#!/usr/bin/env python3
"""eval_semantic_live_gate.py — Contract C1: the SHADOW->LIVE promotion gate
for semantic recall (HU_SEMANTIC_RECALL=off|shadow|live).

Background (AlpsBench, arXiv 2603.26680): adding memory retrieval improves
persona awareness but DEGRADES emotional intelligence and real-vs-hypothetical
("reality") awareness — models over-rely on retrieved memories. Per
.claude/rules/feature-gate-requires-measurement.md, semantic recall may not be
promoted OFF->SHADOW->LIVE without a measurement; this script IS that
measurement for the SHADOW->LIVE step.

What it does
------------
1. Selects a FIXED set of >= --min-n (default 30) real inbound contexts from a
   real ground-truth corpus (default: a local ~/.human archive of real iMessage
   incoming/reply pairs). Selection is deterministic (sha256-sorted) so the
   same corpus always yields the same contexts.
2. Generates a reply per context TWICE against the live realtime server
   (default http://127.0.0.1:8741), using the PRODUCTION system prompt
   (eval_blinded_ab.production_system_prompt(), via tools/dump_prompt_head):
     - Arm A (SHADOW): system prompt unmodified.
     - Arm B (LIVE): the top --top-k `human memory search --semantic <ctx>`
       results are prepended to the system prompt as a "Relevant memories:"
       block — this is what LIVE would inject; the daemon's own in-process
       recall is not addressable from Python, so the retrieval is reproduced
       via the real C retrieval path (hu_semantic_retrieve) against a COPY of
       the live memory.db, never the live db itself.
   All requests carry `X-HU-Priority: batch` — the server is PRODUCTION.
3. Scores every reply with the real C anti-AI scorer (`human eval score`,
   ground truth: hu_shape_classify), called PER-REPLY so every context has its
   own recorded anti_ai value (not just an arm-wide mean), AND a Gemini judge
   (Vertex ADC, gemini-3.1-pro-preview, explicit thinkingConfig.thinkingBudget
   on every call, responseSchema) rating 1-5 on:
     - emotional_intelligence: does the reply respond to the FEELING behind
       the incoming message, not just its literal content?
     - reality_awareness: does the reply keep hypothetical scenarios and other
       people's facts separate from the user's own real situation?
4. PAIRING (the part a naive per-arm comparison gets wrong): SHADOW and LIVE
   are generated independently, and either arm can fail a given context
   (timeout, empty completion, search failure). The two arms are compared
   ONLY on the INTERSECTION of contexts where BOTH produced a scored reply —
   never on "however many happened to succeed per arm". A context that
   succeeded in one arm and not the other is recorded (shadow_only/live_only)
   but excluded from the composite/EI/reality comparison, because comparing
   arm-wide means computed over DIFFERENT context sets is not a measurement of
   LIVE vs SHADOW — it is a measurement of which contexts survived, which is
   exactly the "identical numbers because nothing was actually compared"
   failure shape in .claude/rules/reports-success-does-nothing.md.
5. RECALL COVERAGE: if `human memory search --semantic` returned zero results
   for most contexts, the LIVE arm's prompt is barely different from SHADOW's,
   and any resemblance between the two arms proves nothing about semantic
   recall specifically. recall_coverage = fraction of PAIRED contexts where a
   non-empty memories block was actually appended in the LIVE arm. Below
   --min-recall-coverage (default 0.5) the verdict is forced to INCONCLUSIVE
   regardless of the composite/EI/reality comparison.
6. Composes a per-arm composite (humanness_compose.compute_composite) over the
   PAIRED set and compares LIVE against SHADOW. PROMOTE only if the composite
   did not drop AND neither EI nor reality-awareness dropped (beyond a small
   noise tolerance), AND recall coverage was adequate. Otherwise HOLD.

Per .claude/rules/no-number-without-a-measurement.md and
reports-success-does-nothing.md, this script REFUSES (exit 2, writes nothing)
rather than emit a number it cannot stand behind:
  - fewer than --min-n PAIRED scored replies (both arms succeeded)
  - fewer than --min-n judge (EI/reality) scores in the paired set, per arm
  - the embedder preflight fails (HU_SEMANTIC_EMBED_URL unreachable)
  - the Gemini judge preflight fails (no ADC/API key, or unreachable)
  - the memory.db copy cannot be made
  - fewer than --min-n usable contexts exist in the corpus

The output JSON carries a per-context row for every PAIRED context (id,
recall_bytes, ei, reality, anti_ai for each arm) plus per-arm EI/reality score
histograms — no reply text, no incoming-message text; every number in the
verdict is traceable to a specific context id.

Stdlib only, except pytest for the sibling test file. No writes to
~/.human/config.json, no service restarts, no writes to the live memory.db
(a COPY is made under /tmp and used for search).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import humanness_compose as hc  # noqa: E402
import eval_blinded_ab as eab  # noqa: E402  (production_system_prompt, ADC token helper)

REPO_ROOT = HERE.parent

# --------------------------------------------------------------------------
# Defaults
# --------------------------------------------------------------------------
DEFAULT_CONTEXTS = os.path.expanduser(
    "~/.human/logs/eval-archive/ground_truth-backup-20260725-113527.jsonl")
DEFAULT_SERVER = "http://127.0.0.1:8741"
DEFAULT_EMBED_URL = os.environ.get("HU_SEMANTIC_EMBED_URL", "http://127.0.0.1:8749")
DEFAULT_MEMORY_DB = os.path.expanduser("~/.human/memory.db")
DEFAULT_MODEL = "seth-glm-air"
DEFAULT_N = 40           # requested contexts; kept above MIN_SCORED for headroom
DEFAULT_MIN_N = 30       # contract floor (applies to the PAIRED set)
DEFAULT_TOP_K = 5
DEFAULT_MAX_TOKENS = 120
DEFAULT_TEMPERATURE = 0.7
DEFAULT_COMPOSITE_TOLERANCE = 0.02
DEFAULT_EI_TOLERANCE = 0.15     # on the 1-5 judge scale
DEFAULT_REALITY_TOLERANCE = 0.15
DEFAULT_MIN_RECALL_COVERAGE = 0.5
PRIORITY_HEADER = {"X-HU-Priority": "batch"}

GEMINI_API_KEY = os.environ.get("GEMINI_API_KEY", "")
GEMINI_PROJECT_ID = os.environ.get("GOOGLE_CLOUD_PROJECT", "johnb-2025")
GEMINI_MODEL = "gemini-3.1-pro-preview"

# gemini-3.x shares maxOutputTokens between invisible thinking and the visible
# reply (CLAUDE.md gotcha) — an unset budget can starve the JSON body. Mirrors
# scripts/eval_blinded_ab.py's JUDGE_THINKING_BUDGET. Set unconditionally on
# EVERY judge call (see judge_gen_config) — there is no code path that omits it.
JUDGE_THINKING_BUDGET = 1024
JUDGE_MAX_OUTPUT_TOKENS = 2048

_EI_JUDGE_SCHEMA = {
    "type": "object",
    "properties": {
        "emotional_intelligence": {"type": "integer", "minimum": 1, "maximum": 5},
        "reality_awareness": {"type": "integer", "minimum": 1, "maximum": 5},
        "reasoning": {"type": "string"},
    },
    "required": ["emotional_intelligence", "reality_awareness", "reasoning"],
    "propertyOrdering": ["emotional_intelligence", "reality_awareness", "reasoning"],
}

_EI_PROMPT_TEMPLATE = """You are an expert judge of interpersonal texting quality.

Incoming message: {incoming!r}
Reply being scored: {reply!r}

Score the REPLY on two dimensions, 1 (worst) to 5 (best):

1. emotional_intelligence: does the reply respond to the FEELING behind the
   incoming message, not only to its literal content? A reply that answers
   the words but ignores an obvious emotional subtext (stress, excitement,
   grief, affection) should score low even if factually correct.

2. reality_awareness: does the reply keep hypothetical scenarios, other
   people's facts, and the sender's own situation separate from the user's
   own real life and facts? A reply that treats a hypothetical as real, or
   confuses someone else's situation for the user's own, should score low.
   A reply with nothing hypothetical to confuse should score 5 by default.

Return integers only, with brief reasoning.
"""


# --------------------------------------------------------------------------
# Gemini judge (Vertex ADC pattern, mirrors eval_blinded_ab.py / eval_humanness.py)
# --------------------------------------------------------------------------
_adc_token_cache = {"token": None, "expires": 0}


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
    req = urllib.request.Request("https://oauth2.googleapis.com/token",
                                 data=payload, headers={"Content-Type": "application/x-www-form-urlencoded"})
    resp = urllib.request.urlopen(req, timeout=10)
    data = json.loads(resp.read())
    _adc_token_cache["token"] = data["access_token"]
    _adc_token_cache["expires"] = time.time() + data.get("expires_in", 3600)
    return data["access_token"]


def _gemini_url():
    if GEMINI_API_KEY:
        return (f"https://generativelanguage.googleapis.com/v1beta/models/"
                f"{GEMINI_MODEL}:generateContent?key={GEMINI_API_KEY}")
    return (f"https://aiplatform.googleapis.com/v1/projects/{GEMINI_PROJECT_ID}/locations/global/"
            f"publishers/google/models/{GEMINI_MODEL}:generateContent")


def judge_gen_config(temperature, response_schema=None):
    """thinkingConfig.thinkingBudget is set on EVERY call, schema or not —
    there is no path through this function that omits it."""
    cfg = {
        "temperature": temperature,
        "maxOutputTokens": JUDGE_MAX_OUTPUT_TOKENS,
        "thinkingConfig": {"thinkingBudget": JUDGE_THINKING_BUDGET},
    }
    if response_schema is not None:
        cfg["responseMimeType"] = "application/json"
        cfg["responseSchema"] = response_schema
    return cfg


def _fake_gemini_response(prompt):
    """HU_GATE_FAKE=1 short-circuit — deterministic, network-free, varies with
    input so tests can distinguish arms/contexts without a real judge."""
    h = int(hashlib.sha256(prompt.encode("utf-8")).hexdigest(), 16)
    ei = 1 + (h % 5)
    reality = 1 + ((h // 5) % 5)
    return json.dumps({
        "emotional_intelligence": ei,
        "reality_awareness": reality,
        "reasoning": "HU_GATE_FAKE=1 canned response",
    })


def call_gemini(prompt, temperature=0.2, response_schema=None, timeout=30):
    if os.environ.get("HU_GATE_FAKE") == "1":
        return _fake_gemini_response(prompt)
    gen_cfg = judge_gen_config(temperature, response_schema)
    payload = json.dumps({
        "contents": [{"role": "user", "parts": [{"text": prompt}]}],
        "generationConfig": gen_cfg,
    }).encode()
    headers = {"Content-Type": "application/json"}
    if not GEMINI_API_KEY:
        token = _get_adc_token()
        if not token:
            raise RuntimeError("No GEMINI_API_KEY and no ADC credentials found")
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(_gemini_url(), data=payload, headers=headers)
    resp = urllib.request.urlopen(req, timeout=timeout)
    data = json.loads(resp.read())
    return data["candidates"][0]["content"]["parts"][0]["text"]


def _loads_json_lenient(raw):
    s = raw
    if "```json" in s:
        s = s.split("```json")[1].split("```")[0].strip()
    elif "```" in s:
        s = s.split("```")[1].split("```")[0].strip()
    return json.loads(s)


def judge_ei_reality(incoming, reply):
    """Returns {"ei": int, "reality": int} or None on any failure (network,
    parse, out-of-range). Callers must treat None as "this reply has no judge
    score", not as a fatal error — a few per-item failures are normal."""
    prompt = _EI_PROMPT_TEMPLATE.format(incoming=incoming, reply=reply)
    try:
        raw = call_gemini(prompt, temperature=0.2, response_schema=_EI_JUDGE_SCHEMA)
        data = _loads_json_lenient(raw)
        ei = int(data["emotional_intelligence"])
        reality = int(data["reality_awareness"])
        if not (1 <= ei <= 5 and 1 <= reality <= 5):
            return None
        return {"ei": ei, "reality": reality}
    except Exception:  # noqa: BLE001 — one bad judgment must not kill the run
        return None


def preflight_judge():
    try:
        r = judge_ei_reality("hey you doing ok? you seemed off today",
                             "yeah just tired, work's been a lot. thanks for checking though")
        return r is not None
    except Exception:  # noqa: BLE001
        return False


# --------------------------------------------------------------------------
# Embedder preflight + real generation
# --------------------------------------------------------------------------
def preflight_embedder(embed_url, timeout=30):
    body = json.dumps({"input": "preflight"}).encode()
    req = urllib.request.Request(embed_url.rstrip("/") + "/v1/embeddings", data=body,
                                 headers={"Content-Type": "application/json", **PRIORITY_HEADER})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read())
        return bool(data.get("data"))
    except Exception:  # noqa: BLE001
        return False


def generate(server, model, system_prompt, message, max_tokens, temperature, timeout=120):
    messages = [{"role": "system", "content": system_prompt}, {"role": "user", "content": message}]
    body = json.dumps({
        "model": model,
        "messages": messages,
        "max_tokens": max_tokens,
        "temperature": temperature,
    }).encode()
    req = urllib.request.Request(server.rstrip("/") + "/v1/chat/completions", data=body,
                                 headers={"Content-Type": "application/json", **PRIORITY_HEADER})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        data = json.loads(resp.read())
    return data["choices"][0]["message"]["content"].strip()


# --------------------------------------------------------------------------
# Context selection (fixed, deterministic subset of a real corpus)
# --------------------------------------------------------------------------
def select_contexts(path, n, min_len=4, max_len=280):
    p = Path(path).expanduser()
    if not p.is_file():
        return []
    texts = []
    seen = set()
    for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        text = (row.get("incoming") or row.get("prompt") or "").strip()
        if not (min_len <= len(text) <= max_len):
            continue
        if text in seen:
            continue
        seen.add(text)
        texts.append(text)
    # Deterministic, order-independent-of-file-position selection: sort by a
    # stable hash so re-runs against the same corpus always pick the same
    # contexts, and growth of the corpus doesn't silently reshuffle everything.
    texts.sort(key=lambda t: hashlib.sha256(t.encode("utf-8")).hexdigest())
    return texts[:n]


# --------------------------------------------------------------------------
# memory.db copy (never touch the live db)
# --------------------------------------------------------------------------
def copy_memory_db(src, dst_dir):
    src = os.path.expanduser(src)
    if not os.path.isfile(src):
        return None
    dst_dir = Path(dst_dir)
    dst_dir.mkdir(parents=True, exist_ok=True)
    dst = dst_dir / "memory.db"
    sqlite3_bin = shutil.which("sqlite3")
    ok = False
    if sqlite3_bin:
        try:
            proc = subprocess.run([sqlite3_bin, src, f".backup {dst}"],
                                  capture_output=True, text=True, timeout=180)
            ok = proc.returncode == 0 and dst.is_file()
        except Exception:  # noqa: BLE001
            ok = False
    if not ok:
        try:
            shutil.copy2(src, dst)
            ok = dst.is_file()
        except Exception:  # noqa: BLE001
            ok = False
    return str(dst) if ok else None


# --------------------------------------------------------------------------
# Semantic search (arm B retrieval), via the real C CLI path
# --------------------------------------------------------------------------
def _parse_semantic_results(stdout):
    if stdout.strip().startswith("No results"):
        return []
    import re
    chunks = re.split(r"(?=^\s*\[\d+\]\s)", stdout, flags=re.MULTILINE)
    out = []
    for chunk in chunks:
        m = re.match(r"^\s*\[(\d+)\]\s+(.*?)\s+\((-?[0-9.]+)\):\s(.*)$", chunk, re.DOTALL)
        if not m:
            continue
        content = m.group(4).strip()
        if content:
            out.append(content)
    return out


def semantic_search(human_bin, memory_db, embed_url, query, k, timeout=90):
    """Returns a list of up to `k` memory snippets, [] for no results, or
    None on infra failure (binary missing, timeout, non-zero exit) — callers
    must treat None as "could not measure LIVE for this context", not as
    "no memories", or the LIVE arm would silently degrade toward SHADOW."""
    if not human_bin or not os.path.isfile(human_bin):
        return None
    env = dict(os.environ)
    env["HU_MEMORY_SQLITE_PATH"] = memory_db
    env["HU_SEMANTIC_EMBED_URL"] = embed_url
    try:
        # errors="replace": the CLI cuts each hit at 2000 bytes (%.*s) and can
        # split a multi-byte UTF-8 sequence; strict decoding killed the
        # 2026-09-03 rerun at LIVE 11/40. A mangled byte becomes U+FFFD.
        proc = subprocess.run([human_bin, "memory", "search", "--semantic", query],
                              capture_output=True, encoding="utf-8", errors="replace",
                              timeout=timeout, env=env)
    except (subprocess.TimeoutExpired, OSError):
        return None
    if proc.returncode != 0:
        return None
    return _parse_semantic_results(proc.stdout)[:k]


# Byte budget for the recall block — MIRRORS the in-binary clamp
# (hu_semantic_recall_clamp_result, src/memory/semantic_recall.c): per-hit
# content cut at a word boundary to RECALL_HIT_MAX_BYTES, hits kept in rank
# order only while the cumulative content stays within the budget. Without
# this the LIVE arm injected up to 5 x 2000-char hits (the CLI prints up to
# 2000 bytes per hit) and 9/40 contexts returned EMPTY completions on
# 2026-09-02. Keep these two constants in sync with semantic_recall.h.
RECALL_HIT_MAX_BYTES = 240
DEFAULT_RECALL_MAX_BYTES = 1200


def recall_max_bytes():
    v = os.environ.get("HU_SEMANTIC_RECALL_MAX_BYTES", "")
    try:
        n = int(v)
    except ValueError:
        return DEFAULT_RECALL_MAX_BYTES
    return n if n > 0 else DEFAULT_RECALL_MAX_BYTES


def truncate_hit_bytes(s, max_bytes):
    """Word-boundary byte truncation, same rule as hu_semantic_recall_truncate_len:
    the last whitespace in the upper half of the window, else a hard cut that
    never splits a UTF-8 sequence."""
    b = s.encode("utf-8")
    if len(b) <= max_bytes:
        return s
    for i in range(max_bytes, max_bytes // 2, -1):
        if b[i] in b" \n\t":
            cut = i
            while cut > 0 and b[cut - 1] in b" \n\t":
                cut -= 1
            return b[:cut].decode("utf-8", errors="ignore")
    return b[:max_bytes].decode("utf-8", errors="ignore")


# Content filter — MIRRORS hu_semantic_recall_hit_is_excluded
# (src/memory/semantic_recall.c). After the byte clamp, 6/40 LIVE contexts on
# 2026-09-02 still returned EMPTY completions; single requests isolated the
# trigger to the CONTENT of the top hits: episodic "Task:/Actions:/Outcome:/
# Score:" scaffolding from the experience writer, and hits that are an
# AI-identity confrontation ("are you texting or your ai??", "Is this Seth").
# Cues match at WORD boundaries, case-insensitively ("ai" must not fire inside
# "said"/"wait"/"maid"); bare "AI" as a topic is deliberately not a cue. Keep
# this list identical to AI_IDENTITY_CUES in semantic_recall.c.
AI_IDENTITY_CUES = [
    "your ai", "is an ai", "are an ai", "be an ai",
    "was an ai", "you're an ai", "youre an ai", "you're ai",
    "you are ai", "is the ai", "are the ai", "ai generated",
    "ai-generated", "generated by ai", "ai wrote", "a bot",
    "chatbot", "chat bot", "a robot", "automated message",
    "automated reply", "auto reply", "auto-reply", "is this really",
    "is this actually", "is that really", "is that actually", "is this you",
    "is that you", "is it really you", "is it actually you", "are you real",
    "are you human", "who is this", "who's this", "whos this",
    "who am i texting", "am i texting", "who am i talking to", "am i talking to",
    "talking to a machine",
]
_SCAFFOLD_RE = re.compile(r"\ATask: .*?\nActions: ", re.DOTALL)
_IDENTITY_Q_RE = re.compile(r"\A\s*is (this|that) ", re.IGNORECASE)


def _contains_word_ci(hay, needle):
    return re.search(r"(?<![A-Za-z0-9])" + re.escape(needle) + r"(?![A-Za-z0-9])",
                     hay, re.IGNORECASE) is not None


def hit_is_excluded(snippet):
    """True when a semantic hit must not be injected into the reply prompt."""
    if not snippet:
        return False
    if _SCAFFOLD_RE.match(snippet):
        return True
    if any(_contains_word_ci(snippet, cue) for cue in AI_IDENTITY_CUES):
        return True
    # A bare identity question ("Is this Seth"): <= 4 words opening "is this"/"is that".
    return bool(_IDENTITY_Q_RE.match(snippet)) and len(re.findall(r"[A-Za-z0-9]+", snippet)) <= 4


def build_memories_block(snippets):
    """Returns (block_or_None, n_dropped): excluded hits are dropped BEFORE
    the byte budget so they never consume it, mirroring the LIVE branch in
    src/memory/retrieval/hybrid.c (filter, then clamp)."""
    if not snippets:
        return None, 0
    survivors = [s for s in snippets if not hit_is_excluded(s)]
    dropped = len(snippets) - len(survivors)
    budget = recall_max_bytes()
    used = 0
    kept = []
    for s in survivors:
        t = truncate_hit_bytes(s, RECALL_HIT_MAX_BYTES)
        n = len(t.encode("utf-8"))
        if used + n > budget:
            break
        used += n
        kept.append(t)
    if not kept:
        return None, dropped
    lines = "\n".join(f"- {s}" for s in kept)
    return f"Relevant memories:\n{lines}\n\n", dropped


# --------------------------------------------------------------------------
# Scoring: `human eval score` (ground-truth C scorer), called PER-REPLY so
# every context carries its own anti_ai value, not just an arm-wide mean.
# --------------------------------------------------------------------------
def score_arm(human_bin, rows, timeout=90):
    """rows: list of {"reply":..., "channel":...}. Returns the raw
    `human eval score` JSON doc, or None on failure. One call can score
    any number of rows (used both for single-reply and batch scoring)."""
    if not rows:
        return None
    if not human_bin or not os.path.isfile(human_bin):
        return None
    jsonl = "\n".join(json.dumps({"reply": r["reply"], "channel": r.get("channel", "imessage")})
                      for r in rows) + "\n"
    try:
        proc = subprocess.run([human_bin, "eval", "score", "--in", "/dev/stdin"],
                              input=jsonl, capture_output=True, text=True, timeout=timeout)
    except (subprocess.TimeoutExpired, OSError):
        return None
    if proc.returncode != 0:
        return None
    try:
        return json.loads(proc.stdout.strip().splitlines()[-1])
    except (json.JSONDecodeError, IndexError):
        return None


def score_single_reply_anti_ai(human_bin, reply, channel):
    """The real C shape/anti-AI scorer (hu_shape_classify) for ONE reply, so
    every context can carry its own per-reply anti_ai value. Returns None on
    scoring failure — the caller must not fabricate a fallback score."""
    doc = score_arm(human_bin, [{"reply": reply, "channel": channel}])
    if doc is None:
        return None
    return doc.get("axes", {}).get("anti_ai", {}).get("mean")


# --------------------------------------------------------------------------
# Arm runner — returns a dict keyed by context id (index into `contexts`),
# containing ONLY the contexts that produced a real, non-empty, scored reply.
# A context absent from the returned dict FAILED that arm (search failure,
# generation exception, empty completion) — see the per-context `reasons`
# dict for why, so failures are attributable, not silently dropped.
# --------------------------------------------------------------------------
def run_arm(arm_name, contexts, system_prompt, args, memory_db_path, log=print):
    results = {}
    fail_reasons = {}
    for i, ctx in enumerate(contexts):
        sp = system_prompt
        recall_bytes = 0
        recall_dropped = 0
        if arm_name == "live":
            snippets = semantic_search(args.human_bin, memory_db_path, args.embed_url,
                                       ctx, args.top_k)
            if snippets is None:
                fail_reasons[i] = "semantic_search_failed"
                log(f"  [warn][{arm_name}] semantic search failed, skipping context "
                    f"{i}: {ctx[:50]!r}", file=sys.stderr, flush=True)
                continue
            block, recall_dropped = build_memories_block(snippets)
            if block:
                sp = block + system_prompt
                recall_bytes = len(block.encode("utf-8"))
        try:
            reply = generate(args.server, args.model, sp, ctx, args.max_tokens, args.temperature)
        except Exception as e:  # noqa: BLE001 — one bad context must not kill a 40-context run
            fail_reasons[i] = f"generation_exception: {e}"
            log(f"  [warn][{arm_name}] generation failed for context {i}: {e}",
               file=sys.stderr, flush=True)
            continue
        if not reply:
            fail_reasons[i] = "empty_reply"
            log(f"  [warn][{arm_name}] empty reply for context {i}", file=sys.stderr, flush=True)
            continue
        j = judge_ei_reality(ctx, reply)
        anti_ai = score_single_reply_anti_ai(args.human_bin, reply, args.channel)
        results[i] = {
            "recall_bytes": recall_bytes,
            "recall_dropped": recall_dropped,
            "ei": (j["ei"] if j else None),
            "reality": (j["reality"] if j else None),
            "anti_ai": anti_ai,
        }
        log(f"  [{arm_name}] {i+1}/{len(contexts)}  {reply[:60]!r}", flush=True)
    return results, fail_reasons


# --------------------------------------------------------------------------
# Pairing + per-arm summary over the PAIRED set only
# --------------------------------------------------------------------------
def paired_ids(shadow_results, live_results):
    """The only ids eligible for the SHADOW-vs-LIVE comparison: both arms
    produced a scored reply. Comparing arm-wide means computed over DIFFERENT
    context sets is not a measurement of LIVE vs SHADOW."""
    return sorted(set(shadow_results) & set(live_results))


def _mean(vals):
    return statistics.fmean(vals) if vals else 0.0


def _stderr(vals):
    if len(vals) <= 1:
        return 0.0
    return statistics.pstdev(vals) / (len(vals) ** 0.5)


def _histogram_1to5(vals):
    return {str(k): vals.count(k) for k in range(1, 6)}


def summarize_paired_arm(results, ids):
    """results: run_arm's per-context dict. ids: the PAIRED id list. Every
    number here is reconstructable from the per-context rows written to the
    output JSON (same `ids`, same per-context values)."""
    rows = [results[i] for i in ids]
    anti_ai_vals = [r["anti_ai"] for r in rows if r["anti_ai"] is not None]
    ei_vals = [r["ei"] for r in rows if r["ei"] is not None]
    reality_vals = [r["reality"] for r in rows if r["reality"] is not None]
    anti_ai_mean = _mean(anti_ai_vals)
    ei_mean = _mean(ei_vals)
    reality_mean = _mean(reality_vals)
    axes = {
        "anti_ai": {"mean": anti_ai_mean, "stderr": _stderr(anti_ai_vals), "n": len(anti_ai_vals)},
        "judge": {"mean": (ei_mean - 1.0) / 4.0 if ei_vals else 0.0, "stderr": 0.0, "n": len(ei_vals)},
    }
    composite, used_weights = hc.compute_composite(axes)
    return {
        "n": len(ids),
        "n_anti_ai": len(anti_ai_vals),
        "n_ei": len(ei_vals),
        "n_reality": len(reality_vals),
        "composite": composite,
        "composite_weights": used_weights,
        "anti_ai_mean": anti_ai_mean,
        "anti_ai_stderr": _stderr(anti_ai_vals),
        "ei_mean": ei_mean,
        "ei_histogram": _histogram_1to5(ei_vals),
        "reality_mean": reality_mean,
        "reality_histogram": _histogram_1to5(reality_vals),
    }


def recall_coverage_of(live_results, ids):
    if not ids:
        return 0.0
    hit = sum(1 for i in ids if live_results.get(i, {}).get("recall_bytes", 0) > 0)
    return hit / len(ids)


def build_context_rows(shadow_results, live_results, ids):
    rows = []
    for i in ids:
        s, l = shadow_results[i], live_results[i]  # noqa: E741
        rows.append({
            "id": i,
            "recall_bytes": l["recall_bytes"],
            "recall_dropped": l.get("recall_dropped", 0),
            "shadow": {"ei": s["ei"], "reality": s["reality"], "anti_ai": s["anti_ai"]},
            "live": {"ei": l["ei"], "reality": l["reality"], "anti_ai": l["anti_ai"]},
        })
    return rows


# --------------------------------------------------------------------------
# Verdict logic (pure — the target of the unit tests)
# --------------------------------------------------------------------------
def decide_verdict(shadow, live, recall_coverage,
                   composite_tolerance=DEFAULT_COMPOSITE_TOLERANCE,
                   ei_tolerance=DEFAULT_EI_TOLERANCE,
                   reality_tolerance=DEFAULT_REALITY_TOLERANCE,
                   min_recall_coverage=DEFAULT_MIN_RECALL_COVERAGE):
    """PROMOTE only if (a) recall coverage was high enough that this run
    actually exercised LIVE's difference from SHADOW, and (b) LIVE does not
    regress SHADOW on composite, EI, or reality-awareness (each within a
    small noise tolerance). Pure function: no I/O, unit-tested directly."""
    if recall_coverage < min_recall_coverage:
        return "INCONCLUSIVE", [
            f"recall coverage {recall_coverage:.3f} < min {min_recall_coverage:.3f} — semantic "
            f"search returned nothing for most paired contexts, so LIVE's prompt barely "
            f"differed from SHADOW's; this run does not test what it claims to test"]

    reasons = []
    ok = True

    if live["composite"] < shadow["composite"] - composite_tolerance:
        ok = False
        reasons.append(
            f"composite dropped: live={live['composite']:.4f} < "
            f"shadow={shadow['composite']:.4f} - tol={composite_tolerance:.4f}")

    if live["ei_mean"] < shadow["ei_mean"] - ei_tolerance:
        ok = False
        reasons.append(
            f"emotional_intelligence dropped: live={live['ei_mean']:.3f} < "
            f"shadow={shadow['ei_mean']:.3f} - tol={ei_tolerance:.3f} "
            f"(AlpsBench: memory retrieval degrades EI)")

    if live["reality_mean"] < shadow["reality_mean"] - reality_tolerance:
        ok = False
        reasons.append(
            f"reality_awareness dropped: live={live['reality_mean']:.3f} < "
            f"shadow={shadow['reality_mean']:.3f} - tol={reality_tolerance:.3f} "
            f"(AlpsBench: memory retrieval degrades real-vs-hypothetical awareness)")

    return ("PROMOTE" if ok else "HOLD"), reasons


def build_system_prompt(args):
    if args.system_prompt_file:
        sp = Path(args.system_prompt_file).expanduser().read_text().strip()
        if not sp:
            raise SystemExit("FATAL: --system-prompt-file is empty; refusing to score against "
                              "an empty prompt.")
        return sp
    if args.dump_prompt_head_bin:
        os.environ.setdefault("HU_DUMP_PROMPT_HEAD", args.dump_prompt_head_bin)
    # production_system_prompt() raises SystemExit on failure — this script
    # inherits that "refuse rather than fall back to an authored prompt"
    # contract from eval_blinded_ab.py; no separate handling needed here.
    return eab.production_system_prompt(persona=args.persona, channel=args.channel,
                                        contact=args.contact)


def refuse(reason):
    print(f"REFUSE: {reason}", file=sys.stderr)
    print("(no gate JSON written — .claude/rules/no-number-without-a-measurement.md)",
         file=sys.stderr)
    return 2


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--contexts", default=DEFAULT_CONTEXTS,
                    help="JSONL of real inbound contexts (field 'incoming' or 'prompt')")
    ap.add_argument("--n", type=int, default=DEFAULT_N, help="contexts to request")
    ap.add_argument("--min-n", type=int, default=DEFAULT_MIN_N,
                    help="minimum PAIRED scored replies (both arms succeeded), and minimum "
                         "judge scores within the paired set per arm, below which the run "
                         "REFUSES rather than emit a verdict")
    ap.add_argument("--min-recall-coverage", type=float, default=DEFAULT_MIN_RECALL_COVERAGE,
                    help="fraction of paired contexts that must have gotten a non-empty "
                         "semantic-search block in the LIVE arm; below this the verdict is "
                         "INCONCLUSIVE (a JSON IS written) rather than PROMOTE/HOLD")
    ap.add_argument("--server", default=DEFAULT_SERVER)
    ap.add_argument("--embed-url", default=DEFAULT_EMBED_URL)
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--memory-db", default=DEFAULT_MEMORY_DB,
                    help="live memory.db to COPY (never opened directly)")
    ap.add_argument("--human-bin", default=str(REPO_ROOT / "build/human"))
    ap.add_argument("--dump-prompt-head-bin", default=None,
                    help="override for HU_DUMP_PROMPT_HEAD (see eval_blinded_ab.py)")
    ap.add_argument("--persona", default="seth")
    ap.add_argument("--channel", default="imessage")
    ap.add_argument("--contact", default="-")
    ap.add_argument("--system-prompt-file", default=None,
                    help="skip production_system_prompt() and use this file's contents instead")
    ap.add_argument("--top-k", type=int, default=DEFAULT_TOP_K)
    ap.add_argument("--max-tokens", type=int, default=DEFAULT_MAX_TOKENS)
    ap.add_argument("--temperature", type=float, default=DEFAULT_TEMPERATURE)
    ap.add_argument("--composite-tolerance", type=float, default=DEFAULT_COMPOSITE_TOLERANCE)
    ap.add_argument("--ei-tolerance", type=float, default=DEFAULT_EI_TOLERANCE)
    ap.add_argument("--reality-tolerance", type=float, default=DEFAULT_REALITY_TOLERANCE)
    ap.add_argument("--out", default=None,
                    help="verdict JSON path (default: "
                         "docs/plans/2026-08-02-semantic-retrieval/semantic-live-gate-<date>.json)")
    ap.add_argument("--tmp-dir", default=None, help="scratch dir for the memory.db copy")
    args = ap.parse_args(argv)

    if args.out is None:
        date = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        args.out = str(REPO_ROOT / "docs/plans/2026-08-02-semantic-retrieval" /
                      f"semantic-live-gate-{date}.json")

    print(f"[1/6] embedder preflight ({args.embed_url}) ...", flush=True)
    if not preflight_embedder(args.embed_url):
        return refuse(f"embedder unreachable at {args.embed_url}/v1/embeddings")

    print("[2/6] judge preflight (Vertex ADC / gemini-3.1-pro-preview) ...", flush=True)
    if not preflight_judge():
        return refuse("Gemini judge unreachable (no ADC/API key, or the endpoint failed)")

    print(f"[3/6] copying memory.db from {args.memory_db} ...", flush=True)
    tmp_root = args.tmp_dir or tempfile.mkdtemp(prefix="hu_semantic_gate_")
    memory_db_path = copy_memory_db(args.memory_db, tmp_root)
    if not memory_db_path:
        return refuse(f"could not copy memory db from {args.memory_db} to {tmp_root}")
    print(f"      -> {memory_db_path}", flush=True)

    print(f"[4/6] selecting >= {args.min_n} real inbound contexts from {args.contexts} ...",
         flush=True)
    contexts = select_contexts(args.contexts, args.n)
    if len(contexts) < args.min_n:
        return refuse(f"only {len(contexts)} usable contexts found in {args.contexts} "
                      f"(< --min-n {args.min_n})")
    print(f"      -> {len(contexts)} contexts selected", flush=True)

    try:
        system_prompt = build_system_prompt(args)
    except SystemExit as e:
        return refuse(str(e))
    print(f"[5/6] production system prompt: {len(system_prompt)} chars", flush=True)

    print("[6/6] generating + scoring both arms (SHADOW, then LIVE) ...", flush=True)
    shadow_results, shadow_fail = run_arm("shadow", contexts, system_prompt, args, memory_db_path)
    live_results, live_fail = run_arm("live", contexts, system_prompt, args, memory_db_path)

    ids = paired_ids(shadow_results, live_results)
    shadow_only = sorted(set(shadow_results) - set(live_results))
    live_only = sorted(set(live_results) - set(shadow_results))
    print(f"      paired={len(ids)}  shadow_only={len(shadow_only)}  live_only={len(live_only)}",
         flush=True)

    if len(ids) < args.min_n:
        return refuse(f"only {len(ids)} contexts succeeded in BOTH arms (< --min-n {args.min_n}); "
                      f"shadow_only={shadow_only} live_only={live_only} "
                      f"shadow_fail_reasons={shadow_fail} live_fail_reasons={live_fail}")

    shadow_summary = summarize_paired_arm(shadow_results, ids)
    live_summary = summarize_paired_arm(live_results, ids)

    if shadow_summary["n_anti_ai"] < args.min_n:
        return refuse(f"SHADOW arm produced {shadow_summary['n_anti_ai']} `human eval score` "
                      f"anti_ai scores in the paired set (< {args.min_n}) — the C scorer "
                      f"(`human eval score`) may be unavailable or failing")
    if live_summary["n_anti_ai"] < args.min_n:
        return refuse(f"LIVE arm produced {live_summary['n_anti_ai']} `human eval score` "
                      f"anti_ai scores in the paired set (< {args.min_n}) — the C scorer "
                      f"(`human eval score`) may be unavailable or failing")
    if shadow_summary["n_ei"] < args.min_n:
        return refuse(f"SHADOW arm produced {shadow_summary['n_ei']} judge scores in the paired "
                      f"set (< {args.min_n}) — judge may have degraded mid-run")
    if live_summary["n_ei"] < args.min_n:
        return refuse(f"LIVE arm produced {live_summary['n_ei']} judge scores in the paired "
                      f"set (< {args.min_n}) — judge may have degraded mid-run")

    coverage = recall_coverage_of(live_results, ids)
    verdict, reasons = decide_verdict(shadow_summary, live_summary, coverage,
                                      args.composite_tolerance, args.ei_tolerance,
                                      args.reality_tolerance, args.min_recall_coverage)

    try:
        git_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True,
                                             cwd=str(REPO_ROOT)).strip()
    except Exception:  # noqa: BLE001
        git_commit = None

    doc = {
        "schema": "semantic_live_gate.v2",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "git_commit": git_commit,
        "gate": "HU_SEMANTIC_RECALL shadow->live",
        "n_contexts": len(contexts),
        "n_paired": len(ids),
        "n_shadow_only": len(shadow_only),
        "n_live_only": len(live_only),
        "shadow_only_ids": shadow_only,
        "live_only_ids": live_only,
        "shadow_fail_reasons": shadow_fail,
        "live_fail_reasons": live_fail,
        "recall_coverage": coverage,
        "min_recall_coverage": args.min_recall_coverage,
        "contexts_source": os.path.expanduser(args.contexts),
        "server": args.server,
        "embed_url": args.embed_url,
        "top_k": args.top_k,
        "judge_thinking_budget": JUDGE_THINKING_BUDGET,
        "tolerances": {
            "composite": args.composite_tolerance,
            "ei": args.ei_tolerance,
            "reality": args.reality_tolerance,
        },
        "shadow": shadow_summary,
        "live": live_summary,
        "context_rows": build_context_rows(shadow_results, live_results, ids),
        "verdict": verdict,
        "reasons": reasons,
    }
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(doc, indent=2) + "\n")

    print(json.dumps({k: v for k, v in doc.items() if k != "context_rows"}, indent=2))
    print(f"\nSEMANTIC LIVE GATE VERDICT: {verdict}")
    for r in reasons:
        print(f"  - {r}")
    print(f"Written: {out_path}")
    return 0 if verdict == "PROMOTE" else 1


if __name__ == "__main__":
    sys.exit(main())
