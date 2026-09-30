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

Stdlib only, except pytest for the sibling test file (and reusing
scripts/second_opinion/backend.GemmaBackend's stdlib-only HTTP client for the
local judge). No writes to ~/.human/config.json, no service restarts, no
writes to the live memory.db (a COPY is made under /tmp and used for search).

The EI/reality judge backend (--judge-backend, default "local") is separate
from the second-opinion-lane policy this mirrors: local Gemma 4 31B on
127.0.0.1:8743 never leaves the Mac; Vertex Gemini is opt-in (--judge-backend
vertex) and prints a stderr notice every time it is used, because it sends
incoming-message text and generated replies to Google Cloud.
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
# The owner's standing policy (2026-09-29, second_opinion/backend.py): any job
# that reads real message text defaults to the LOCAL Gemma judge; Vertex is
# opt-in only, behind --judge-backend vertex, with a stderr notice on every
# use. Reuse GemmaBackend's own HTTP client rather than re-implementing it.
from second_opinion.backend import GemmaBackend, GEMMA_MODEL, GEMMA_PORT, GEMMA_PYTHON  # noqa: E402

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
# Below this fraction of PAIRED --fusion contexts whose retrieved context
# (memories block) differs between arms, the run is forced INCONCLUSIVE — see
# decide_verdict() and contexts_differing_fraction(). 0.05 is deliberately
# low: a genuinely-applied fusion merge (rrf vs score) reorders or drops
# overlapping candidates on nearly every multi-hit query, so a real treatment
# clears 5% by a wide margin. A run at or near 0.0 is not a weak effect, it is
# the "identical values to full precision" / treatment-never-applied tell
# (.claude/rules/no-number-without-a-measurement.md) — the textbook shape is
# both arms silently falling back to keyword-only (see
# SEMANTIC_UNAVAILABLE_MARKER) and therefore producing byte-identical
# contexts regardless of which fusion mode was requested.
DEFAULT_MIN_DIFF_FRAC = 0.05
REGISTER_MAX_CASUAL_WORDS = 12  # mirrors semantic_recall.h; keep in sync
PRIORITY_HEADER = {"X-HU-Priority": "batch"}

# The EXACT stderr text `human memory search --hybrid` prints when it could
# not attach the semantic index and fell back to keyword-only retrieval
# (src/app/cli_commands.c, the `search --hybrid` branch: `fprintf(stderr,
# "search --hybrid: semantic index unavailable, using keyword only\n")`).
# hybrid_search() greps THIS marker in the CLI child's stderr, not the
# returncode: the fallback still returns 0 and can still produce non-empty
# (keyword-only) results, so a returncode/empty-result check alone cannot see
# it — that gap is exactly what let a fusion pair PASS while both arms
# silently ran the same keyword-only path (see run_arm()). If the wording at
# that call site ever changes, this constant must change with it.
SEMANTIC_UNAVAILABLE_MARKER = "semantic index unavailable"

# hu_hybrid_retrieve's graph-boost / typed-seeding path (spreading-activation
# and graph-rerank-boost rows, merged in as an extra retrieval source) is
# fed by an in-memory hu_graph_t that only a long-lived daemon process
# populates via store(); every query this gate issues runs in a fresh,
# short-lived `human memory search --hybrid` CLI child, which always passes
# graph=NULL (src/app/cli_commands.c: `hu_hybrid_retrieve(..., NULL, hq,
# ...)`; see the comment at src/memory/retrieval/hybrid.c documenting the
# same gap for the offline alpha sweep). A PROMOTE/HOLD from this gate is
# therefore silent on the graph-boost interaction — not a bug in the gate,
# a limitation of measuring the CLI path instead of a live daemon. Recorded
# in the output JSON (`limitations`) and the printed summary rather than
# building a daemon harness to close it.
GRAPH_BOOST_LIMITATION = (
    "graph-boost / typed-seeding recall (spreading-activation + graph-rerank-boost rows) is "
    "NOT measured by this gate: hu_hybrid_retrieve's `graph` argument is hard-NULL from "
    "`human memory search --hybrid` (src/app/cli_commands.c) because the in-memory graph "
    "index is populated only by store() inside a long-lived daemon process, and every query "
    "here runs in a fresh, short-lived CLI child process (see the comment in "
    "src/memory/retrieval/hybrid.c documenting the same gap for the offline alpha sweep). A "
    "PROMOTE/HOLD verdict says nothing about the graph-boost interaction."
)

GEMINI_API_KEY = os.environ.get("GEMINI_API_KEY", "")
GEMINI_PROJECT_ID = os.environ.get("GOOGLE_CLOUD_PROJECT", "johnb-2025")
GEMINI_MODEL = "gemini-3.1-pro-preview"

# Local judge (default backend) — Gemma 4 31B served locally by mlx_lm.server,
# same model/port/python as the second-opinion lane (scripts/second_opinion/
# backend.py). Never leaves the Mac.
LOCAL_JUDGE_BASE_URL = f"http://127.0.0.1:{GEMMA_PORT}"
LOCAL_JUDGE_MODEL = GEMMA_MODEL
# The exact command that starts the server (mirrors backend.py:serve_gemma's
# own `cmd` list) — printed verbatim in the preflight-refusal message so an
# operator can copy-paste it rather than go spelunking for the recipe.
LOCAL_JUDGE_START_CMD = (
    f"{GEMMA_PYTHON} -m mlx_lm.server --model {GEMMA_MODEL} --host 127.0.0.1 "
    f"--port {GEMMA_PORT}"
)

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
# Local judge (default backend) — Gemma 4 31B via GemmaBackend, no
# responseSchema (mlx_lm.server doesn't offer one), so the prompt demands
# strict JSON and the parse is defensive: first balanced {...} object in the
# text, ints 1..5. Parse failure or an out-of-range value returns None —
# NEVER a default/fabricated score (.claude/rules/no-number-without-a-measurement.md)
# — which the caller already treats as "no judge score for this item" and
# counts toward the existing --min-n refusal, same as a Vertex judge miss.
# --------------------------------------------------------------------------
_LOCAL_EI_SYSTEM = "You are an expert judge of interpersonal texting quality."

_LOCAL_EI_PROMPT_TEMPLATE = """Incoming message: {incoming!r}
Reply being scored: {reply!r}

Score the REPLY on two dimensions, integers 1 (worst) to 5 (best):

1. emotional_intelligence: does the reply respond to the FEELING behind the
   incoming message, not only to its literal content? A reply that answers
   the words but ignores an obvious emotional subtext (stress, excitement,
   grief, affection) should score low even if factually correct.

2. reality_awareness: does the reply keep hypothetical scenarios, other
   people's facts, and the sender's own situation separate from the user's
   own real life and facts? A reply with nothing hypothetical to confuse
   should score 5 by default.

Respond with ONLY a single strict JSON object and nothing else — no markdown
fences, no reasoning, no prose before or after it:
{{"emotional_intelligence": <int 1-5>, "reality_awareness": <int 1-5>}}
"""


def _first_json_object(text):
    """Return the first balanced {...} substring in text, or None. Gemma has
    no responseSchema, so the model can (and sometimes does) wrap the JSON in
    a sentence or a code fence; this scans past any prefix rather than
    assuming the response starts with '{'."""
    start = text.find("{")
    while start != -1:
        depth = 0
        for i in range(start, len(text)):
            ch = text[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return text[start:i + 1]
        start = text.find("{", start + 1)
    return None


def judge_ei_reality_local(incoming, reply, backend):
    """Local-Gemma equivalent of judge_ei_reality(): returns {"ei": int,
    "reality": int} or None on ANY failure (network, parse, out-of-range) —
    same contract as the Vertex judge, so callers (run_arm's --min-n
    accounting) don't need to know which backend produced the score."""
    prompt = _LOCAL_EI_PROMPT_TEMPLATE.format(incoming=incoming, reply=reply)
    try:
        raw = backend.generate(_LOCAL_EI_SYSTEM, prompt, max_tokens=200)
        blob = _first_json_object(raw)
        if blob is None:
            return None
        data = json.loads(blob)
        ei = int(data["emotional_intelligence"])
        reality = int(data["reality_awareness"])
        if not (1 <= ei <= 5 and 1 <= reality <= 5):
            return None
        return {"ei": ei, "reality": reality}
    except Exception:  # noqa: BLE001 — one bad judgment must not kill the run
        return None


def _local_judge_healthy(base_url, timeout=10):
    """GET {base_url}/v1/models — mlx_lm.server (see backend.py:serve_gemma,
    which starts this same server) answers this and /health with HTTP 200
    once the model is loaded. True only on a 200; any failure (connection
    refused, DNS, timeout, non-200) is unhealthy — the caller reports the
    actionable start command, not the raw exception."""
    try:
        with urllib.request.urlopen(base_url.rstrip("/") + "/v1/models", timeout=timeout) as r:
            return r.status == 200
    except Exception:  # noqa: BLE001
        return False


def preflight_judge_local(base_url):
    return _local_judge_healthy(base_url)


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
def select_contexts(path, n, min_len=4, max_len=280, register=None):
    """register: None/"any" keeps every context; "casual"/"substantive" keeps only
    contexts on that side of the 12-word register boundary (US-5 sizing: a
    register-gate run needs enough CASUAL contexts, a substantive-only run needs
    enough long ones). Filtering happens before the deterministic sha256 ordering."""
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
        if register in ("casual", "substantive") and classify_register(text) != register:
            continue
        seen.add(text)
        texts.append(text)
    # Deterministic, order-independent-of-file-position selection: sort by a
    # stable hash so re-runs against the same corpus always pick the same
    # contexts, and growth of the corpus doesn't silently reshuffle everything.
    texts.sort(key=lambda t: hashlib.sha256(t.encode("utf-8")).hexdigest())
    return texts[:n]


# --------------------------------------------------------------------------
# Register classification (mirrors src/memory/semantic_recall.c:hu_semantic_recall_register_admits)
# --------------------------------------------------------------------------
def classify_register(text):
    """Classify a context as 'casual' (<=12 words) or 'substantive' (>12 words).
    Mirrors the C predicate's word-count rule (whitespace-run split, i.e. Python
    str.split() semantics). Keep in lock-step with semantic_recall.h, same
    discipline as hit_is_excluded's existing C<->Python mirror."""
    return "casual" if len(text.split()) <= REGISTER_MAX_CASUAL_WORDS else "substantive"


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


def _cli_search(human_bin, memory_db, embed_url, search_args, k, extra_env=None, timeout=90):
    """Run `human memory search <search_args>` against the db COPY. Returns
    (results_or_None, stderr_text): results is None on any infra failure
    (binary missing, timeout, non-zero exit); stderr_text is always the raw
    captured stderr (possibly "") so callers can detect a keyword-only
    fallback the CLI reports via stderr even on an otherwise-successful
    (returncode 0, well-formed results) call — see
    SEMANTIC_UNAVAILABLE_MARKER and hybrid_search()."""
    if not human_bin or not os.path.isfile(human_bin):
        return None, ""
    env = dict(os.environ)
    env["HU_MEMORY_SQLITE_PATH"] = memory_db
    env["HU_SEMANTIC_EMBED_URL"] = embed_url
    env.update(extra_env or {})
    try:
        # errors="replace": the CLI cuts each hit at 2000 bytes (%.*s) and can
        # split a multi-byte UTF-8 sequence; strict decoding killed the
        # 2026-09-03 rerun at LIVE 11/40. A mangled byte becomes U+FFFD.
        proc = subprocess.run([human_bin, "memory", "search", *search_args],
                              capture_output=True, encoding="utf-8", errors="replace",
                              timeout=timeout, env=env)
    except (subprocess.TimeoutExpired, OSError):
        return None, ""
    if proc.returncode != 0:
        return None, (proc.stderr or "")
    return _parse_semantic_results(proc.stdout)[:k], (proc.stderr or "")


def semantic_search(human_bin, memory_db, embed_url, query, k, timeout=90):
    """Returns a list of up to `k` memory snippets, [] for no results, or
    None on infra failure (binary missing, timeout, non-zero exit) — callers
    must treat None as "could not measure LIVE for this context", not as
    "no memories", or the LIVE arm would silently degrade toward SHADOW."""
    results, _stderr = _cli_search(human_bin, memory_db, embed_url, ["--semantic", query], k,
                                   None, timeout)
    return results


def hybrid_search(human_bin, memory_db, embed_url, query, k, fusion_env, timeout=90):
    """The daemon memory loader's call (`memory search --hybrid --plain`) under
    `fusion_env` (see fusion_env_for_arm). Returns (results_or_None,
    semantic_unavailable): semantic_unavailable is True iff the CLI's stderr
    carried SEMANTIC_UNAVAILABLE_MARKER, meaning this call silently ran
    keyword-only regardless of returncode/results — callers (run_arm) MUST
    treat that as an invalidated call for the --fusion pair, not a success,
    or a fusion A/B degrades to an unnoticed A/A on the keyword path."""
    results, stderr = _cli_search(human_bin, memory_db, embed_url, ["--hybrid", "--plain", query],
                                  k, fusion_env, timeout)
    return results, (SEMANTIC_UNAVAILABLE_MARKER in stderr)


def fusion_env_for_arm(arm_name, fusion, alpha):
    """--fusion pair (HU_HYBRID_FUSION promotion gate): BOTH arms recall through
    the plain hybrid call with semantic recall LIVE (so the dense leg gets the
    production content filter + byte clamp before the merge); the SHADOW arm is
    the production rrf merge, the LIVE arm is `fusion` at `alpha`. `--fusion rrf`
    makes the two arms identical: an A/A run that measures the gate's noise."""
    env = {"HU_SEMANTIC_RECALL": "live", "HU_HYBRID_FUSION": "rrf"}
    if arm_name == "live":
        env["HU_HYBRID_FUSION"] = fusion
        if fusion == "score":
            env["HU_HYBRID_FUSION_ALPHA"] = f"{alpha:.2f}"
    return env


def validate_fusion_args(fusion, alpha, register_gate):
    """None when the fusion arguments are usable, else the refusal reason."""
    if fusion is None:
        return None if alpha is None else "--alpha needs --fusion score"
    if register_gate != "off":
        return "--fusion measures the merge; run it with --register-gate off"
    if fusion == "score" and (alpha is None or not (0.0 <= alpha <= 1.0)):
        return f"--fusion score needs --alpha in [0,1], got {alpha!r}"
    if fusion == "rrf" and alpha is not None:
        return "--alpha applies to --fusion score only"
    return None


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
def recall_mode_for_arm(arm_name, register_gate, fusion=None):
    """Which contexts get the 'Relevant memories:' block in an arm.

    register_gate off/shadow (the original C1 pair): shadow = "none", live = "all".
    register_gate live (US-5): BOTH arms run semantic recall LIVE and the pair differs
    ONLY on casual contexts — shadow = "all" (register gate OFF), live = "admitted"
    (register gate LIVE: casual contexts have the block withheld). A pair whose shadow
    arm carried no recall at all cannot see the register gate: the 2026-09-05 first run
    produced casual-arm scores identical to 16 digits in both arms and an INCONCLUSIVE
    whose coverage denominator counted the very contexts the gate suppresses by design."""
    if fusion is not None:  # fusion pair: both arms inject; they differ in the merge only
        return "all"
    if arm_name != "live":
        return "all" if register_gate == "live" else "none"
    return "admitted" if register_gate == "live" else "all"


def run_arm(arm_name, contexts, system_prompt, args, memory_db_path, registers=None, log=print,
            recall_mode=None):
    """Generate + score one arm. recall_mode (see recall_mode_for_arm): "none" — no
    semantic search; "all" — inject the block for every context; "admitted" — search
    every context but WITHHOLD the block for casual ones, recording the bytes withheld
    in recall_suppressed_bytes so the gate's effect is measured, not assumed."""
    if recall_mode is None:
        recall_mode = recall_mode_for_arm(arm_name, getattr(args, "register_gate", "off"),
                                          getattr(args, "fusion", None))
    register_gate = getattr(args, "register_gate", "off")
    results = {}
    fail_reasons = {}
    for i, ctx in enumerate(contexts):
        sp = system_prompt
        recall_bytes = 0
        recall_dropped = 0
        recall_suppressed_bytes = 0
        semantic_unavailable_here = False
        context_hash = None
        if recall_mode != "none":
            casual = registers is not None and registers.get(i) == "casual"
            # AC-5.3: register gate SHADOW -> log but do NOT suppress
            if arm_name == "live" and casual and register_gate == "shadow":
                log(f"  [live] {i}: register gate SHADOW would suppress casual "
                    f"(words<={REGISTER_MAX_CASUAL_WORDS})", flush=True)
            fusion = getattr(args, "fusion", None)
            if fusion is not None:
                snippets, semantic_unavailable_here = hybrid_search(
                    args.human_bin, memory_db_path, args.embed_url, ctx, args.top_k,
                    fusion_env_for_arm(arm_name, fusion, args.alpha))
                if semantic_unavailable_here:
                    log(f"  [warn][{arm_name}] {i}: {SEMANTIC_UNAVAILABLE_MARKER!r} in CLI "
                        f"stderr — this call ran keyword-only, invalidating the --fusion "
                        f"comparison for this context", file=sys.stderr, flush=True)
            else:
                snippets = semantic_search(args.human_bin, memory_db_path, args.embed_url,
                                           ctx, args.top_k)
            if snippets is None:
                fail_reasons[i] = ("hybrid_search_failed" if fusion is not None
                                   else "semantic_search_failed")
                log(f"  [warn][{arm_name}] semantic search failed, skipping context "
                    f"{i}: {ctx[:50]!r}", file=sys.stderr, flush=True)
                continue
            block, recall_dropped = build_memories_block(snippets)
            # The actual retrieved context injected into the prompt (or lack of
            # one), hashed rather than stored verbatim to keep the "no reply
            # text, no incoming-message text" discipline the output JSON already
            # follows — see contexts_differing_fraction(), which compares this
            # across arms to catch a fusion A/B that retrieved the same thing
            # for both arms (the treatment-never-applied tell).
            context_hash = hashlib.sha256((block or "").encode("utf-8")).hexdigest()
            if block:
                if recall_mode == "admitted" and casual:
                    # AC-5.1: register gate LIVE + casual -> block withheld; its size is
                    # the evidence that there was something to suppress.
                    recall_suppressed_bytes = len(block.encode("utf-8"))
                    log(f"  [live] {i}: register gate LIVE suppresses casual "
                        f"(words<={REGISTER_MAX_CASUAL_WORDS}); {recall_suppressed_bytes} "
                        f"bytes withheld", flush=True)
                else:
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
        # main() attaches the resolved backend call as args._judge_call so
        # run_arm() doesn't need to know local vs vertex; direct callers of
        # run_arm() (several unit tests build a bare Args() with no such
        # attribute) fall back to judge_ei_reality unchanged, preserving
        # their existing monkeypatch-judge_ei_reality behavior exactly.
        judge_call = getattr(args, "_judge_call", None) or judge_ei_reality
        j = judge_call(ctx, reply)
        anti_ai = score_single_reply_anti_ai(args.human_bin, reply, args.channel)
        results[i] = {
            "recall_bytes": recall_bytes,
            "recall_dropped": recall_dropped,
            "recall_suppressed_bytes": recall_suppressed_bytes,
            "ei": (j["ei"] if j else None),
            "reality": (j["reality"] if j else None),
            "anti_ai": anti_ai,
            "semantic_unavailable": semantic_unavailable_here,
            "context_hash": context_hash,
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


def semantic_unavailable_ids(results):
    """ids (within one arm's run_arm() output) whose CLI call reported
    SEMANTIC_UNAVAILABLE_MARKER — this call ran keyword-only despite
    returncode 0 and well-formed results, so it must not be trusted as a
    measurement of the requested --fusion mode."""
    return sorted(i for i, r in results.items() if r.get("semantic_unavailable"))


def contexts_differing_fraction(shadow_results, live_results, ids):
    """Fraction of PAIRED ids (--fusion pair only) whose retrieved context —
    the memories block actually injected into the prompt, compared by hash
    (see run_arm's context_hash) — differs between arms. A run where the
    fusion mode changed almost nothing that was retrieved measured the
    A/A noise floor, not the fusion mode under test; see DEFAULT_MIN_DIFF_FRAC
    and decide_verdict()."""
    if not ids:
        return 0, 0, 0.0
    differing = sum(1 for i in ids
                    if shadow_results.get(i, {}).get("context_hash") !=
                       live_results.get(i, {}).get("context_hash"))
    total = len(ids)
    return differing, total, differing / total


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


def register_gate_coverage(shadow_results, ids, registers):
    """register_gate=live coverage: the fraction of paired CASUAL contexts whose
    gate-OFF arm actually carried a recall block — the only contexts on which the
    register gate changes the prompt. Below --min-recall-coverage the run is
    INCONCLUSIVE ("nothing to suppress on this corpus"), which is a finding, not a
    failure. Counting suppressed casual contexts as zero-coverage (the 2026-09-05 first
    run) makes the verdict INCONCLUSIVE by construction."""
    casual = [i for i in ids if registers.get(i) == "casual"]
    return recall_coverage_of(shadow_results, casual)


def register_summary(shadow_results, live_results, reg_ids, coverage, min_n,
                     composite_tolerance=DEFAULT_COMPOSITE_TOLERANCE,
                     ei_tolerance=DEFAULT_EI_TOLERANCE,
                     reality_tolerance=DEFAULT_REALITY_TOLERANCE,
                     min_recall_coverage=DEFAULT_MIN_RECALL_COVERAGE):
    """Per-register breakdown. A register with fewer than min_n paired contexts gets
    INCONCLUSIVE, never PROMOTE/HOLD (the first run said PROMOTE on n=7)."""
    if not reg_ids:
        return {"n": 0, "verdict": "EMPTY"}
    s = summarize_paired_arm(shadow_results, reg_ids)
    l = summarize_paired_arm(live_results, reg_ids)  # noqa: E741
    if len(reg_ids) < min_n:
        verdict, reasons = "INCONCLUSIVE", [
            f"only {len(reg_ids)} paired contexts in this register (< min_n {min_n}) — "
            f"no per-register verdict"]
    else:
        verdict, reasons = decide_verdict(s, l, coverage, composite_tolerance, ei_tolerance,
                                          reality_tolerance, min_recall_coverage)
    return {
        "n": len(reg_ids),
        "shadow": s,
        "live": l,
        "recall_coverage": coverage,
        "verdict": verdict,
        "reasons": reasons,
    }


def build_context_rows(shadow_results, live_results, ids, registers=None):
    rows = []
    for i in ids:
        s, l = shadow_results[i], live_results[i]  # noqa: E741
        row = {
            "id": i,
            "recall_bytes": l["recall_bytes"],
            "recall_dropped": l.get("recall_dropped", 0),
            "shadow_recall_bytes": s.get("recall_bytes", 0),
            "recall_suppressed_bytes": l.get("recall_suppressed_bytes", 0),
            "shadow": {"ei": s["ei"], "reality": s["reality"], "anti_ai": s["anti_ai"]},
            "live": {"ei": l["ei"], "reality": l["reality"], "anti_ai": l["anti_ai"]},
        }
        if registers is not None:
            row["register"] = registers.get(i, "unknown")
        rows.append(row)
    return rows


# --------------------------------------------------------------------------
# Verdict logic (pure — the target of the unit tests)
# --------------------------------------------------------------------------
def decide_verdict(shadow, live, recall_coverage,
                   composite_tolerance=DEFAULT_COMPOSITE_TOLERANCE,
                   ei_tolerance=DEFAULT_EI_TOLERANCE,
                   reality_tolerance=DEFAULT_REALITY_TOLERANCE,
                   min_recall_coverage=DEFAULT_MIN_RECALL_COVERAGE,
                   diff_frac=None, min_diff_frac=DEFAULT_MIN_DIFF_FRAC):
    """PROMOTE only if (a) recall coverage was high enough that this run
    actually exercised LIVE's difference from SHADOW, (a2) for a --fusion
    pair, the retrieved context actually differed between arms often enough
    (diff_frac; pass None to skip this check — the non-fusion pairs have no
    equivalent signal), and (b) LIVE does not regress SHADOW on composite,
    EI, or reality-awareness (each within a small noise tolerance). Pure
    function: no I/O, unit-tested directly."""
    if recall_coverage < min_recall_coverage:
        return "INCONCLUSIVE", [
            f"recall coverage {recall_coverage:.3f} < min {min_recall_coverage:.3f} — semantic "
            f"search returned nothing for most paired contexts, so LIVE's prompt barely "
            f"differed from SHADOW's; this run does not test what it claims to test"]

    if diff_frac is not None and diff_frac < min_diff_frac:
        return "INCONCLUSIVE", [
            f"contexts_differing fraction {diff_frac:.3f} < --min-diff-frac {min_diff_frac:.3f} "
            f"— the requested fusion mode barely changed what was retrieved versus the rrf "
            f"baseline for most paired contexts; comparing replies would measure noise, not "
            f"the fusion mode under test (see SEMANTIC_UNAVAILABLE_MARKER — a common cause is "
            f"both arms silently falling back to keyword-only)"]

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
    ap.add_argument("--register-gate", choices=["off", "shadow", "live"], default="off",
                    help="register-conditioned gate mode: off (default: the SHADOW-vs-LIVE "
                         "semantic-recall pair), shadow (that pair, logging what the gate "
                         "would suppress), or live (BOTH arms run semantic recall LIVE and "
                         "the pair differs only on casual contexts, where the LIVE arm "
                         "withholds the block; coverage is then measured on casual contexts "
                         "that actually retrieved something)")
    ap.add_argument("--register-select", choices=["any", "casual", "substantive"], default="any",
                    help="restrict the fixed context selection to one register (sizing aid)")
    ap.add_argument("--composite-tolerance", type=float, default=DEFAULT_COMPOSITE_TOLERANCE)
    ap.add_argument("--ei-tolerance", type=float, default=DEFAULT_EI_TOLERANCE)
    ap.add_argument("--reality-tolerance", type=float, default=DEFAULT_REALITY_TOLERANCE)
    ap.add_argument("--out", default=None,
                    help="verdict JSON path (default: "
                         "docs/plans/2026-08-02-semantic-retrieval/semantic-live-gate-<date>.json)")
    ap.add_argument("--tmp-dir", default=None, help="scratch dir for the memory.db copy")
    ap.add_argument("--fusion", choices=["rrf", "score"], default=None,
                    help="HU_HYBRID_FUSION promotion pair: both arms recall via `memory search "
                         "--hybrid --plain`; SHADOW = rrf (production), LIVE = this mode. "
                         "`rrf` gives an A/A noise run")
    ap.add_argument("--alpha", type=float, default=None,
                    help="HU_HYBRID_FUSION_ALPHA for the LIVE arm (required with --fusion score)")
    ap.add_argument("--min-diff-frac", type=float, default=DEFAULT_MIN_DIFF_FRAC,
                    help="--fusion pair only: minimum fraction of paired contexts whose "
                         "retrieved context must differ between arms; below this the verdict "
                         "is forced INCONCLUSIVE (see DEFAULT_MIN_DIFF_FRAC for why 0.05)")
    ap.add_argument("--dry-run", action="store_true",
                    help="parse arguments and initialize, but skip generation/scoring")
    ap.add_argument("--judge-backend", choices=["local", "vertex"], default="local",
                    help="EI/reality judge backend. 'local' (default): Gemma 4 31B served "
                         "on this Mac (127.0.0.1) — never sends message text off the "
                         "machine. 'vertex': Gemini via Vertex ADC — sends incoming "
                         "messages and generated replies to Google Cloud; prints a stderr "
                         "notice on every use. See .claude/rules/second-opinion-lane "
                         "policy (2026-09-29): real-message jobs default to local.")
    ap.add_argument("--judge-local-url", default=LOCAL_JUDGE_BASE_URL,
                    help="base URL for the local Gemma judge server (must be loopback)")
    ap.add_argument("--judge-local-model", default=LOCAL_JUDGE_MODEL,
                    help="model name reported by the local judge server")
    args = ap.parse_args(argv)
    fusion_problem = validate_fusion_args(args.fusion, args.alpha, args.register_gate)
    if fusion_problem:
        return refuse(fusion_problem)

    if args.out is None:
        date = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        stem = "semantic-live-gate" + (f"-fusion-{args.fusion}" if args.fusion else "")
        args.out = str(REPO_ROOT / "docs/plans/2026-08-02-semantic-retrieval" /
                      f"{stem}-{date}.json")

    print(f"[1/6] embedder preflight ({args.embed_url}) ...", flush=True)
    if not preflight_embedder(args.embed_url):
        return refuse(f"embedder unreachable at {args.embed_url}/v1/embeddings")

    # Judge backend selection: local (default) never sends message text off
    # this Mac; vertex is opt-in and notices on stderr every time it's used
    # (second-opinion-lane policy, 2026-09-29). Resolved here rather than at
    # each judge_ei_reality*() call site so run_arm() (and its existing unit
    # tests, which build a bare Args() with no _judge_call) stay unaware of
    # which backend is active.
    if args.judge_backend == "local":
        print(f"[2/6] judge preflight (local Gemma @ {args.judge_local_url}) ...", flush=True)
        if not preflight_judge_local(args.judge_local_url):
            return refuse(
                f"local Gemma judge unreachable at {args.judge_local_url}/v1/models. "
                f"Start it with:\n    {LOCAL_JUDGE_START_CMD}\n"
                f"(see scripts/second_opinion/backend.py:serve_gemma and "
                f"scripts/second_opinion/README.md 'Operator steps'), then re-run — "
                f"or pass --judge-backend vertex to use the cloud judge instead.")
        local_backend = GemmaBackend(base_url=args.judge_local_url, model=args.judge_local_model)
        args._judge_call = (
            lambda incoming, reply: judge_ei_reality_local(incoming, reply, local_backend))
        judge_backend_name, judge_model_name = "local", local_backend.model
    else:
        print(f"second-opinion: backend=vertex; message text (incoming messages and "
              f"generated replies) will leave this Mac for judging (Google Cloud project "
              f"{GEMINI_PROJECT_ID}, ADC)", file=sys.stderr)
        print(f"[2/6] judge preflight (Vertex ADC / {GEMINI_MODEL}) ...", flush=True)
        if not preflight_judge():
            return refuse("Gemini judge unreachable (no ADC/API key, or the endpoint failed)")
        args._judge_call = judge_ei_reality
        judge_backend_name, judge_model_name = "vertex", GEMINI_MODEL

    print(f"[3/6] copying memory.db from {args.memory_db} ...", flush=True)
    tmp_root = args.tmp_dir or tempfile.mkdtemp(prefix="hu_semantic_gate_")
    memory_db_path = copy_memory_db(args.memory_db, tmp_root)
    if not memory_db_path:
        return refuse(f"could not copy memory db from {args.memory_db} to {tmp_root}")
    print(f"      -> {memory_db_path}", flush=True)

    print(f"[4/6] selecting >= {args.min_n} real inbound contexts from {args.contexts} ...",
         flush=True)
    contexts = select_contexts(args.contexts, args.n,
                               register=(None if args.register_select == "any" else args.register_select))
    if len(contexts) < args.min_n:
        return refuse(f"only {len(contexts)} usable contexts found in {args.contexts} "
                      f"(< --min-n {args.min_n})")
    print(f"      -> {len(contexts)} contexts selected", flush=True)

    # Classify contexts by register (US-5: casual <=12 words, substantive >12 words)
    registers = {i: classify_register(ctx) for i, ctx in enumerate(contexts)}
    casual_ids = [i for i, r in registers.items() if r == "casual"]
    substantive_ids = [i for i, r in registers.items() if r == "substantive"]
    print(f"      register split: casual={len(casual_ids)}, substantive={len(substantive_ids)}")

    try:
        system_prompt = build_system_prompt(args)
    except SystemExit as e:
        return refuse(str(e))
    print(f"[5/6] production system prompt: {len(system_prompt)} chars", flush=True)

    if args.dry_run:
        print("[DRY RUN] Skipping generation/scoring (--dry-run flag set)")
        print(json.dumps({"dry_run": True, "n_contexts": len(contexts), "registers": registers},
                        indent=2))
        return 0

    print(f"[6/6] generating + scoring both arms "
          f"(SHADOW={recall_mode_for_arm('shadow', args.register_gate, args.fusion)}, "
          f"then LIVE={recall_mode_for_arm('live', args.register_gate, args.fusion)}) ...",
          flush=True)
    shadow_results, shadow_fail = run_arm("shadow", contexts, system_prompt, args, memory_db_path,
                                          registers=registers)
    live_results, live_fail = run_arm("live", contexts, system_prompt, args, memory_db_path,
                                      registers=registers)

    # A --fusion arm that silently fell back to keyword-only is not a
    # successful measurement of the requested mode, even though its CLI call
    # returned 0 and produced well-formed results (see hybrid_search() /
    # SEMANTIC_UNAVAILABLE_MARKER). Refuse outright rather than let those
    # calls count toward the pairing below — a fusion A/B where both arms
    # ran keyword-only PASSES vacuously otherwise (identical contexts read
    # as "no regression", not "nothing was measured").
    shadow_unavailable = semantic_unavailable_ids(shadow_results)
    live_unavailable = semantic_unavailable_ids(live_results)
    if shadow_unavailable or live_unavailable:
        return refuse(
            f"semantic index reported unavailable ({SEMANTIC_UNAVAILABLE_MARKER!r}) during a "
            f"--fusion run: shadow contexts={shadow_unavailable} live contexts="
            f"{live_unavailable} — a fusion A/B measured on a keyword-only fallback tests the "
            f"CLI's degraded path, not the requested fusion mode; fix the embedder/vector-store "
            f"attach and rerun")

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

    if args.register_gate == "live":
        coverage = register_gate_coverage(shadow_results, ids, registers)
    else:
        coverage = recall_coverage_of(live_results, ids)

    # --fusion pair only: how often did the requested merge mode actually
    # change what was retrieved, versus the rrf baseline? diff_frac stays
    # None for the non-fusion pairs (decide_verdict skips the check then).
    if args.fusion is not None:
        contexts_differing, contexts_total, diff_frac = contexts_differing_fraction(
            shadow_results, live_results, ids)
    else:
        contexts_differing, contexts_total, diff_frac = None, None, None

    verdict, reasons = decide_verdict(shadow_summary, live_summary, coverage,
                                      args.composite_tolerance, args.ei_tolerance,
                                      args.reality_tolerance, args.min_recall_coverage,
                                      diff_frac=diff_frac, min_diff_frac=args.min_diff_frac)

    # AC-5.4: if register_gate is LIVE, verify that casual contexts have zero recall_bytes
    # in the LIVE arm (suppression must actually have happened, per reports-success-does-nothing.md)
    if args.register_gate == "live":
        casual_paired_ids = [i for i in ids if registers.get(i) == "casual"]
        leaking_casual = [i for i in casual_paired_ids if live_results.get(i, {}).get("recall_bytes", 0) > 0]
        if leaking_casual:
            return refuse(f"register gate LIVE but {len(leaking_casual)} casual context(s) "
                         f"{leaking_casual} have recall_bytes > 0 in LIVE arm — suppression failed")

    try:
        git_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True,
                                             cwd=str(REPO_ROOT)).strip()
    except Exception:  # noqa: BLE001
        git_commit = None

    # Build register breakdown (US-5: per-register analysis)
    casual_paired_ids = [i for i in ids if registers.get(i) == "casual"]
    substantive_paired_ids = [i for i in ids if registers.get(i) == "substantive"]

    tol = (args.composite_tolerance, args.ei_tolerance, args.reality_tolerance,
           args.min_recall_coverage)

    def summarize_register(name, reg_ids):
        if args.register_gate == "live" and name == "casual":
            reg_coverage = register_gate_coverage(shadow_results, reg_ids, registers)
        else:
            reg_coverage = recall_coverage_of(live_results, reg_ids)
        return register_summary(shadow_results, live_results, reg_ids, reg_coverage,
                                args.min_n, *tol)

    register_breakdown = {
        "boundary_words": REGISTER_MAX_CASUAL_WORDS,
        "register_gate_mode": args.register_gate,
        "casual": summarize_register("casual", casual_paired_ids),
        "substantive": summarize_register("substantive", substantive_paired_ids),
    }

    doc = {
        "schema": "semantic_live_gate.v2",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "git_commit": git_commit,
        "gate": (f"HU_HYBRID_FUSION rrf->{args.fusion} (both arms recall via memory search "
                 f"--hybrid --plain, semantic recall LIVE; the pair differs in the merge only)"
                 if args.fusion else
                 "HU_SEMANTIC_RECALL_REGISTER_GATE off->live (semantic recall LIVE in both "
                 "arms; the pair differs on casual contexts only)"
                 if args.register_gate == "live" else "HU_SEMANTIC_RECALL shadow->live"),
        "fusion": ({"shadow": fusion_env_for_arm("shadow", args.fusion, args.alpha),
                    "live": fusion_env_for_arm("live", args.fusion, args.alpha)}
                   if args.fusion else None),
        "arms": {"shadow": recall_mode_for_arm("shadow", args.register_gate, args.fusion),
                 "live": recall_mode_for_arm("live", args.register_gate, args.fusion)},
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
        "contexts_differing": contexts_differing,
        "contexts_total": contexts_total,
        "contexts_differing_frac": diff_frac,
        "min_diff_frac": args.min_diff_frac if args.fusion is not None else None,
        "limitations": [GRAPH_BOOST_LIMITATION],
        "contexts_source": os.path.expanduser(args.contexts),
        "server": args.server,
        "embed_url": args.embed_url,
        "top_k": args.top_k,
        # Provenance: which judge produced ei/reality — a local-judged verdict
        # must be distinguishable from a Vertex-judged one downstream.
        "judge_backend": judge_backend_name,
        "judge_model": judge_model_name,
        "judge_thinking_budget": JUDGE_THINKING_BUDGET,
        "tolerances": {
            "composite": args.composite_tolerance,
            "ei": args.ei_tolerance,
            "reality": args.reality_tolerance,
        },
        "shadow": shadow_summary,
        "live": live_summary,
        "context_rows": build_context_rows(shadow_results, live_results, ids, registers),
        "register_breakdown": register_breakdown,
        "verdict": verdict,
        "reasons": reasons,
    }
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(doc, indent=2) + "\n")

    print(json.dumps({k: v for k, v in doc.items() if k != "context_rows"}, indent=2))
    print(f"\njudge backend: {judge_backend_name} ({judge_model_name})")
    print(f"SEMANTIC LIVE GATE VERDICT: {verdict}")
    for r in reasons:
        print(f"  - {r}")
    if args.fusion is not None:
        print(f"contexts_differing/contexts_total: {contexts_differing}/{contexts_total} "
             f"({diff_frac:.3f}, floor {args.min_diff_frac:.3f})")
    print("LIMITATIONS:")
    for lim in doc["limitations"]:
        print(f"  - {lim}")
    print(f"Written: {out_path}")
    return 0 if verdict == "PROMOTE" else 1


if __name__ == "__main__":
    sys.exit(main())
