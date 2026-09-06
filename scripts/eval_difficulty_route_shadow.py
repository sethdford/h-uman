#!/usr/bin/env python3
"""
US-8: Difficulty-based routing SHADOW measurement (AC-8.3 / AC-8.4).

The C side (src/agent/model_router.c, HU_DIFFICULTY_ROUTE=shadow) logs which
substantive CONVERSATIONAL turns it WOULD have promoted to the ANALYTICAL
treatment. This script measures whether that promotion would be better, on
real inbound contexts, before anyone flips the gate:

  arm on_device    : the production path — the realtime server on :8741 with
                     the PRODUCTION persona head (eval_blinded_ab.production_
                     system_prompt), the same head the daemon renders.
  arm cloud_shadow : the ANALYTICAL treatment — Vertex gemini-3.1-pro-preview,
                     the SAME persona head (AC-8.1: the head does not depend on
                     the model), thinkingConfig.thinkingBudget=4096 (matches the
                     shadow selection's thinking_budget in model_router.c).

Both arms see identical contexts (real inbound messages of > 12 words, the C
predicate's boundary, that have a real Seth reply on file). Scoring, per reply:
  - anti_ai via the real C scorer (`human eval score`, hu_shape_classify)
  - emotional_intelligence / reality_awareness via the Gemini judge
  - composite = humanness_compose.compute_composite over the PAIRED set only
  - LUAR-MUD twin via scripts/blind_ab/authorship_gap.py (5v5 PersonalBench
    protocol, bootstrap CI) — the same scorer the promotion gate (US-2) reads.
Gate (decide_gate): PROMOTE only if cloud composite >= on-device composite -
tolerance AND cloud twin >= on-device twin; HOLD otherwise with the numbers
recorded; refuse (exit 2, nothing written) below --min-n paired contexts or
when any axis could not be measured (no-number-without-a-measurement).

Privacy: message text and replies live only in memory and in a $TMPDIR trials
file that is deleted after LUAR scoring; the output JSON carries ids, counts,
scores and lengths only. Requests to :8741 carry X-HU-Priority: batch. Vertex
is reached over ADC bearer auth — never `?key=`. No model is loaded in this
process (LUAR-MUD, ~100 MB, loads in the authorship_gap.py subprocess).
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import eval_semantic_live_gate as G  # noqa: E402  (generate, judge, anti_ai, summaries)
import humanness_compose as hc  # noqa: E402,F401  (composite lives in G.summarize_paired_arm)

SUBSTANTIVE_WORDS = 12                 # == HU_DIFFICULTY_ROUTE_SUBSTANTIVE_WORDS
CLOUD_THINKING_BUDGET = 4096           # == shadow_sel.thinking_budget in model_router.c
CLOUD_MAX_OUTPUT_TOKENS = 4096 + 512   # thinking and the visible reply SHARE this budget
DEFAULT_CORPUS = "~/.human/logs/eval-archive/ground_truth-backup-20260725-113527.jsonl"
DEFAULT_HUMAN_BIN = os.path.expanduser("~/.local/bin/human-daemon")
DEFAULT_EVAL_PYTHON = "/opt/homebrew/bin/python3"   # has torch/transformers for LUAR
DEFAULT_MIN_N = 20
DEFAULT_TOLERANCE = 0.02
AUTHORSHIP_GAP = HERE / "blind_ab" / "authorship_gap.py"


@dataclass
class PairedResult:
    """Result from one paired evaluation (kept for the original tests)."""
    context_id: int
    on_device_humanness: Optional[float]
    cloud_shadow_humanness: Optional[float]
    on_device_twin: Optional[float]
    cloud_shadow_twin: Optional[float]
    on_device_recall_bytes: int
    cloud_shadow_recall_bytes: int


# ---------------------------------------------------------------------------
# Contexts
# ---------------------------------------------------------------------------
def _row_message(row: dict) -> str:
    return (row.get("incoming") or row.get("msg") or row.get("prompt") or "").strip()


def select_context_rows(corpus_path: str, n: int = 20, min_len: int = 4,
                        max_len: int = 2000) -> List[Dict[str, str]]:
    """Substantive (> 12 words) real inbound contexts WITH a real Seth reply
    (needed for the LUAR twin). Deduped, sha256-ordered (deterministic and
    independent of file position, like eval_semantic_live_gate.select_contexts),
    capped at n. Returns [{"incoming", "seth_reply"}]."""
    p = Path(os.path.expanduser(corpus_path))
    if not p.is_file():
        return []
    rows, seen = [], set()
    for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        msg = _row_message(row)
        reply = (row.get("seth_reply") or "").strip()
        if not reply or not (min_len <= len(msg) <= max_len):
            continue
        if len(msg.split()) <= SUBSTANTIVE_WORDS or msg in seen:
            continue
        seen.add(msg)
        rows.append({"incoming": msg, "seth_reply": reply})
    rows.sort(key=lambda r: hashlib.sha256(r["incoming"].encode("utf-8")).hexdigest())
    return rows[:n]


def select_contexts(corpus_path: str, n: int = 20, min_len: int = 50, max_len: int = 2000) -> List[str]:
    """Message-only loader (original interface, kept for the dry-run + tests):
    first n substantive messages in file order, no seth_reply required."""
    contexts: List[str] = []
    p = Path(os.path.expanduser(corpus_path))
    if not p.is_file():
        return contexts
    try:
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if len(contexts) >= n:
                    break
                try:
                    row = json.loads(line.strip())
                except (json.JSONDecodeError, ValueError):
                    continue
                msg = _row_message(row)
                if min_len <= len(msg) <= max_len and len(msg.split()) > SUBSTANTIVE_WORDS:
                    contexts.append(msg)
    except IOError:
        pass
    return contexts


# ---------------------------------------------------------------------------
# Arms
# ---------------------------------------------------------------------------
def generate_on_device(system_prompt: str, message: str, server: str = G.DEFAULT_SERVER,
                       model: str = G.DEFAULT_MODEL, max_tokens: int = 160,
                       temperature: float = 0.7, timeout: int = 120) -> Optional[str]:
    """The production path: realtime server + production head. Requests carry
    X-HU-Priority: batch (eval_semantic_live_gate.generate)."""
    try:
        reply = G.generate(server, model, system_prompt, message, max_tokens, temperature,
                           timeout=timeout)
    except Exception:  # noqa: BLE001 — one bad context must not kill the run
        return None
    return reply.strip() if reply else None


def vertex_url(project_id: str, model: str) -> str:
    """Vertex ADC endpoint. Never `?key=` on this host (src/providers/gemini.c
    hu_gemini_base_is_vertex contract; 150 silent 401s in the 07-25 audit)."""
    return (f"https://aiplatform.googleapis.com/v1/projects/{project_id}/locations/global/"
            f"publishers/google/models/{model}:generateContent")


def cloud_payload(system_prompt: str, message: str, thinking_budget: int = CLOUD_THINKING_BUDGET,
                  max_tokens: int = CLOUD_MAX_OUTPUT_TOKENS, temperature: float = 0.7) -> dict:
    """The ANALYTICAL treatment request: same persona head as systemInstruction,
    explicit thinkingConfig.thinkingBudget (Gemini 3.x shares maxOutputTokens
    between thinking and the reply — an unset budget can eat the whole reply)."""
    if max_tokens <= thinking_budget:
        raise ValueError("max_tokens must exceed thinking_budget or the reply can be empty")
    return {
        "systemInstruction": {"parts": [{"text": system_prompt}]},
        "contents": [{"role": "user", "parts": [{"text": message}]}],
        "generationConfig": {
            "temperature": temperature,
            "maxOutputTokens": max_tokens,
            "thinkingConfig": {"thinkingBudget": thinking_budget},
        },
    }


def _visible_text(data: dict) -> str:
    parts = data["candidates"][0]["content"].get("parts", [])
    return "".join(p.get("text", "") for p in parts if not p.get("thought")).strip()


def generate_cloud_shadow(system_prompt: str, message: str, project_id: str = "johnb-2025",
                          model: str = "gemini-3.1-pro-preview", timeout: int = 180,
                          temperature: float = 0.7) -> Optional[str]:
    """Vertex, ADC bearer, same head, thinking budget 4096. None on any failure."""
    try:
        token = G._get_adc_token()
        if not token:
            return None
        payload = json.dumps(cloud_payload(system_prompt, message, temperature=temperature)).encode()
        req = urllib.request.Request(vertex_url(project_id, model), data=payload, headers={
            "Content-Type": "application/json", "Authorization": f"Bearer {token}"})
        data = json.loads(urllib.request.urlopen(req, timeout=timeout).read())
        text = _visible_text(data)
        return text or None
    except Exception:  # noqa: BLE001
        return None


def run_arm(arm_name: str, rows: List[Dict[str, str]], system_prompt: str, gen_fn, args,
            log=print) -> Tuple[Dict[int, dict], Dict[int, str]]:
    """Generate + score one arm. results[i] has the eval_semantic_live_gate row
    shape (so G.summarize_paired_arm / hc.compute_composite apply unchanged)
    plus the reply kept IN MEMORY for LUAR scoring — never written by this
    function."""
    results: Dict[int, dict] = {}
    fail: Dict[int, str] = {}
    for i, row in enumerate(rows):
        reply = gen_fn(system_prompt, row["incoming"])
        if not reply:
            fail[i] = "generation_failed_or_empty"
            log(f"  [warn][{arm_name}] no reply for context {i}", file=sys.stderr, flush=True)
            continue
        j = G.judge_ei_reality(row["incoming"], reply)
        anti = G.score_single_reply_anti_ai(args.human_bin, reply, args.channel)
        results[i] = {
            "recall_bytes": 0, "recall_dropped": 0, "recall_suppressed_bytes": 0,
            "ei": (j["ei"] if j else None), "reality": (j["reality"] if j else None),
            "anti_ai": anti, "reply": reply,
            "reply_chars": len(reply), "reply_words": len(reply.split()),
        }
        log(f"  [{arm_name}] {i + 1}/{len(rows)}  chars={len(reply)}", flush=True)
    return results, fail


# ---------------------------------------------------------------------------
# LUAR twin (real scorer, subprocess) and gate
# ---------------------------------------------------------------------------
def build_trials(rows: List[Dict[str, str]], results: Dict[int, dict], ids: List[int]) -> List[dict]:
    return [{"real_seth": rows[i]["seth_reply"], "ai_response": results[i]["reply"]} for i in ids]


def score_twin_arm(arm_name: str, rows, results, ids, args, tmpdir: str) -> Optional[dict]:
    """Runs scripts/blind_ab/authorship_gap.py on this arm's paired replies.
    The trials file (message text) lives only under tmpdir and is removed here;
    only the cosine statistics come back."""
    trials_path = os.path.join(tmpdir, f"trials-{arm_name}.json")
    out_path = os.path.join(tmpdir, f"gap-{arm_name}.json")
    with open(trials_path, "w") as fh:
        # authorship_gap.py reads json.load(...)["trials"] — the classifier_trials.json shape.
        json.dump({"trials": build_trials(rows, results, ids)}, fh)
    try:
        cmd = [args.eval_python, str(AUTHORSHIP_GAP), "--trials", trials_path, "--out", out_path,
               "--splits", str(args.splits), "--min-trials", str(min(args.min_n, len(ids))),
               "--seed", str(args.seed), "--chatdb", os.path.expanduser(args.chatdb)]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
        if proc.returncode != 0 or not os.path.isfile(out_path):
            print(f"  [warn] authorship_gap.py rc={proc.returncode} for {arm_name}: "
                  f"{proc.stderr.strip()[-400:]}", file=sys.stderr, flush=True)
            return None
        gap = json.load(open(out_path))
        return {
            "twin_seth_vs_adapter": gap.get("twin_seth_vs_adapter"),
            "ceiling_seth_vs_seth": gap.get("ceiling_seth_vs_seth"),
            "floor_seth_vs_other_humans": gap.get("floor_seth_vs_other_humans"),
            "trials": gap.get("trials"),
        }
    finally:
        for p in (trials_path, out_path):
            try:
                os.remove(p)
            except OSError:
                pass


def score_twin(reply: str, model: str = "mlx-community/LUAR-MUD") -> Optional[float]:
    """Single-reply twin scoring is not a thing the protocol defines (5v5 profiles);
    kept for interface compatibility, always None. Use score_twin_arm."""
    return None


def score_humanness_composite(reply: str) -> Optional[float]:
    """Per-arm composites come from G.summarize_paired_arm; kept for interface
    compatibility, always None."""
    return None


def decide_gate(composite_on_device: Optional[float],
                composite_cloud: Optional[float],
                twin_on_device: Optional[float],
                twin_cloud: Optional[float],
                n_paired: int,
                tolerance: float = DEFAULT_TOLERANCE) -> Dict[str, Any]:
    """
    AC-8.4 gate: PROMOTE only if composite_cloud >= composite_on_device - tolerance
    AND twin_cloud >= twin_on_device (no tolerance).

    Otherwise HOLD and record exact numbers.
    """
    result: Dict[str, Any] = {
        "verdict": "INCONCLUSIVE",
        "reason": "insufficient paired results",
        "n_paired": n_paired,
    }

    if n_paired < DEFAULT_MIN_N:
        return result

    if composite_on_device is None or composite_cloud is None:
        result["reason"] = "missing composite scores"
        return result

    if twin_on_device is None or twin_cloud is None:
        result["reason"] = "missing twin scores"
        return result

    composite_delta = composite_cloud - composite_on_device
    twin_delta = twin_cloud - twin_on_device

    if composite_delta >= -tolerance and twin_delta >= 0:
        result["verdict"] = "PROMOTE"
    else:
        result["verdict"] = "HOLD"

    result["reason"] = f"composite_delta={composite_delta:.4f} (tol={tolerance}), twin_delta={twin_delta:.4f}"
    result["composite_on_device"] = composite_on_device
    result["composite_cloud"] = composite_cloud
    result["twin_on_device"] = twin_on_device
    result["twin_cloud"] = twin_cloud

    return result


# ---------------------------------------------------------------------------
# Preflights
# ---------------------------------------------------------------------------
def server_healthy(server: str, timeout: int = 5) -> bool:
    try:
        with urllib.request.urlopen(f"{server.rstrip('/')}/health", timeout=timeout) as r:
            return r.status == 200
    except Exception:  # noqa: BLE001
        return False


def eval_python_has_torch(py: str) -> bool:
    try:
        return subprocess.run([py, "-c", "import torch, transformers"], capture_output=True,
                              timeout=120).returncode == 0
    except Exception:  # noqa: BLE001
        return False


def refuse(reason: str) -> int:
    print(f"REFUSE: {reason}", file=sys.stderr)
    print("(no result JSON written — .claude/rules/no-number-without-a-measurement.md)",
          file=sys.stderr)
    return 2


def context_rows_numeric(rows, od, cl, ids) -> List[dict]:
    out = []
    for i in ids:
        out.append({
            "id": i, "incoming_words": len(rows[i]["incoming"].split()),
            "seth_reply_chars": len(rows[i]["seth_reply"]),
            "on_device": {k: od[i][k] for k in ("ei", "reality", "anti_ai", "reply_chars", "reply_words")},
            "cloud_shadow": {k: cl[i][k] for k in ("ei", "reality", "anti_ai", "reply_chars", "reply_words")},
        })
    return out


# ---------------------------------------------------------------------------
def build_parser():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", default=DEFAULT_CORPUS,
                    help="JSONL with 'incoming' + 'seth_reply' (real pairs)")
    ap.add_argument("-n", "--num-contexts", type=int, default=20,
                    help="substantive contexts to request (default 20)")
    ap.add_argument("--min-n", type=int, default=DEFAULT_MIN_N,
                    help="minimum PAIRED scored contexts below which the run refuses")
    ap.add_argument("--server", default=G.DEFAULT_SERVER, help="on-device realtime server")
    ap.add_argument("--on-device-model", default=G.DEFAULT_MODEL)
    ap.add_argument("--project-id", default="johnb-2025", help="GCP project for Vertex")
    ap.add_argument("--model", default="gemini-3.1-pro-preview", help="Vertex model (cloud arm)")
    ap.add_argument("--human-bin", default=DEFAULT_HUMAN_BIN if os.path.isfile(DEFAULT_HUMAN_BIN)
                    else str(HERE.parent / "build" / "human"))
    ap.add_argument("--dump-prompt-head-bin", default=None)
    ap.add_argument("--persona", default="seth")
    ap.add_argument("--channel", default="imessage")
    ap.add_argument("--contact", default="-")
    ap.add_argument("--system-prompt-file", default=None)
    ap.add_argument("--max-tokens", type=int, default=160)
    ap.add_argument("--temperature", type=float, default=0.7)
    ap.add_argument("--tolerance", type=float, default=DEFAULT_TOLERANCE)
    ap.add_argument("--eval-python", default=os.environ.get("HU_EVAL_PYTHON", DEFAULT_EVAL_PYTHON),
                    help="interpreter with torch/transformers for authorship_gap.py")
    ap.add_argument("--chatdb", default="~/Library/Messages/chat.db")
    ap.add_argument("--splits", type=int, default=200)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--dry-run", action="store_true",
                    help="load contexts, report counts, no generation")
    ap.add_argument("--output", required=True, help="result JSON path")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    corpus_path = os.path.expanduser(args.corpus)

    contexts = select_contexts(corpus_path, n=args.num_contexts)
    if not contexts:
        print(f"ERROR: No contexts loaded from {corpus_path}", file=sys.stderr)
        return 1
    print(f"Loaded {len(contexts)} substantive contexts", file=sys.stderr)

    if args.dry_run:
        rows = select_context_rows(corpus_path, n=args.num_contexts)
        result = {
            "verdict": "DRY_RUN",
            "reason": "dry-run mode: contexts loaded but no generation performed",
            "contexts_loaded": len(contexts),
            "paired_candidates_with_seth_reply": len(rows),
            "min_words_threshold": SUBSTANTIVE_WORDS,
        }
        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        with open(args.output, "w") as f:
            json.dump(result, f, indent=2)
        print(f"Dry-run report: {args.output}", file=sys.stderr)
        return 0

    rows = select_context_rows(corpus_path, n=args.num_contexts)
    if len(rows) < args.min_n:
        return refuse(f"only {len(rows)} substantive contexts with a real reply (< --min-n {args.min_n})")
    if not server_healthy(args.server):
        return refuse(f"on-device server not healthy at {args.server}")
    if os.environ.get("HU_GATE_FAKE") != "1":
        if not G.preflight_judge():
            return refuse("judge (Vertex) preflight failed")
        if not G._get_adc_token():
            return refuse("no ADC credentials for the cloud arm")
    if not eval_python_has_torch(args.eval_python):
        return refuse(f"--eval-python {args.eval_python} lacks torch/transformers for LUAR")
    try:
        system_prompt = G.build_system_prompt(args)
    except SystemExit as e:
        return refuse(str(e))
    print(f"production persona head: {len(system_prompt)} chars (shared by both arms)", flush=True)

    def od_gen(sp, msg):
        return generate_on_device(sp, msg, args.server, args.on_device_model, args.max_tokens,
                                  args.temperature)

    def cl_gen(sp, msg):
        return generate_cloud_shadow(sp, msg, args.project_id, args.model, temperature=args.temperature)

    print(f"[arms] on_device={args.server} ({args.on_device_model})  cloud_shadow={args.model} "
          f"thinking_budget={CLOUD_THINKING_BUDGET}", flush=True)
    od, od_fail = run_arm("on_device", rows, system_prompt, od_gen, args)
    cl, cl_fail = run_arm("cloud_shadow", rows, system_prompt, cl_gen, args)
    ids = G.paired_ids(od, cl)
    print(f"paired={len(ids)} on_device_only={len(set(od) - set(cl))} "
          f"cloud_only={len(set(cl) - set(od))}", flush=True)
    if len(ids) < args.min_n:
        return refuse(f"only {len(ids)} contexts succeeded in BOTH arms (< --min-n {args.min_n}); "
                      f"on_device_fail={od_fail} cloud_fail={cl_fail}")

    od_sum = G.summarize_paired_arm(od, ids)
    cl_sum = G.summarize_paired_arm(cl, ids)
    for name, s in (("on_device", od_sum), ("cloud_shadow", cl_sum)):
        if s["n_ei"] < args.min_n or s["n_anti_ai"] < args.min_n:
            return refuse(f"{name}: judge/anti_ai scored {s['n_ei']}/{s['n_anti_ai']} of {len(ids)} "
                          f"paired (< --min-n {args.min_n})")

    with tempfile.TemporaryDirectory(prefix="us8-twin-") as tmpdir:
        od_twin = score_twin_arm("on_device", rows, od, ids, args, tmpdir)
        cl_twin = score_twin_arm("cloud_shadow", rows, cl, ids, args, tmpdir)
    if not od_twin or not cl_twin:
        return refuse("LUAR twin could not be scored for both arms (see authorship_gap.py stderr above)")

    gate = decide_gate(od_sum["composite"], cl_sum["composite"],
                       od_twin["twin_seth_vs_adapter"]["mean"], cl_twin["twin_seth_vs_adapter"]["mean"],
                       len(ids), args.tolerance)
    if gate["verdict"] == "INCONCLUSIVE":
        return refuse(f"gate could not decide: {gate['reason']}")

    try:
        git_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True,
                                             cwd=str(HERE.parent)).strip()
    except Exception:  # noqa: BLE001
        git_commit = None
    doc = {
        "schema": "difficulty_route_shadow.v1",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "git_commit": git_commit,
        "gate": "HU_DIFFICULTY_ROUTE shadow->live (ANALYTICAL treatment for substantive CONVERSATIONAL turns)",
        "verdict": gate["verdict"], "reason": gate["reason"],
        "n_contexts": len(rows), "n_paired": len(ids),
        "on_device_fail_reasons": od_fail, "cloud_fail_reasons": cl_fail,
        "arms": {"on_device": {"server": args.server, "model": args.on_device_model},
                 "cloud_shadow": {"model": args.model, "thinking_budget": CLOUD_THINKING_BUDGET,
                                  "max_output_tokens": CLOUD_MAX_OUTPUT_TOKENS}},
        "persona_head_chars": len(system_prompt),
        "tolerance": args.tolerance,
        "on_device": {**od_sum, "twin": od_twin},
        "cloud_shadow": {**cl_sum, "twin": cl_twin},
        "context_rows": context_rows_numeric(rows, od, cl, ids),
        "contexts_source": corpus_path,
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w") as f:
        json.dump(doc, f, indent=2)
    print(f"\nDIFFICULTY ROUTE SHADOW VERDICT: {gate['verdict']} — {gate['reason']}")
    print(f"  composite on_device {od_sum['composite']:.3f} -> cloud {cl_sum['composite']:.3f}; "
          f"EI {od_sum['ei_mean']:.2f} -> {cl_sum['ei_mean']:.2f}; "
          f"twin {od_twin['twin_seth_vs_adapter']['mean']:.3f} -> {cl_twin['twin_seth_vs_adapter']['mean']:.3f}")
    print(f"Written: {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
