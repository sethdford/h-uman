#!/usr/bin/env python3
"""
Layer 5 / Month 1 — Verifier-Driven Test-Time Training (TTT).

For each prompt, generate N candidate responses through the production
gateway, score each with the deterministic shape classifier (and
optionally the LLM-judge), pick the best, return that. Optionally log
(prompt, chosen, rejected) triples for future ORPO/DPO training.

This is the o3/DeepSeek-R1-class capability for personalization:
spend N× inference compute per turn to deliver substantially better
quality. Humans don't get to draft 5 versions of every text and pick
the best — h-uman can. That's "better than human" on text-message
quality per unit of stress.

Architecture per docs/plans/2026-05-11-init-05-verifier-driven-ttt.md:
  prompt → N candidates → shape classifier scores → argmax → reply
                                                  ↓
                              log (prompt, chosen, rejected_k) to dpo_pairs
                              for next ORPO round

Usage:
  python3 scripts/verifier_ttt.py --prompt "you around later?" --n 5
  python3 scripts/verifier_ttt.py --batch eval_suites/imessage_humanness.json --n 5
  python3 scripts/verifier_ttt.py --log-to-sqlite ~/.human/memory.db
"""

import argparse
import json
import sqlite3
import sys
import time
from pathlib import Path
from urllib import error, request

sys.path.insert(0, str(Path(__file__).parent))
from eval_shape_classifier import classify  # noqa: E402

GATEWAY_DEFAULT = "http://127.0.0.1:3006/v1/chat/completions"
SPEAKER_ID_CLF_DEFAULT = "/tmp/seth_speaker_id.json"


def _maybe_load_speaker_id_clf(path: str):
    """Best-effort load of the trained speaker-ID classifier.

    Returns None silently if the classifier file isn't present so callers
    fall back to shape-only argmax. Without this fallback, fresh
    checkouts would need to train the classifier before any verifier
    run, which we don't want to require.
    """
    try:
        from personaeval_speaker_id import load_classifier
        return load_classifier(path)
    except (ImportError, FileNotFoundError, json.JSONDecodeError):
        return None


def generate(prompt: str, gateway: str, temperature: float = 0.9,
             max_tokens: int = 80, timeout: int = 180,
             persona_prompt: str = None) -> tuple[str, float, str]:
    """Generate one candidate response.

    When `persona_prompt` is provided, it's injected as the system
    message. This matters when `gateway` points at MLX direct (port 8741)
    rather than the human gateway (port 3006); the gateway's agent_turn
    pipeline injects persona automatically, but MLX direct does not.
    Without this, MLX-direct TTT produces bare-LLM output, defeating the
    purpose of the verifier.
    """
    messages = []
    if persona_prompt:
        messages.append({"role": "system", "content": persona_prompt})
    messages.append({"role": "user", "content": prompt})
    body = {
        "model": "gemma-4-26b",
        "messages": messages,
        "max_tokens": max_tokens,
        "temperature": temperature,
    }
    data = json.dumps(body).encode("utf-8")
    req = request.Request(gateway, data=data, method="POST",
                          headers={"Content-Type": "application/json"})
    t0 = time.time()
    try:
        with request.urlopen(req, timeout=timeout) as r:
            resp = json.loads(r.read())
        try:
            return resp["choices"][0]["message"]["content"].strip(), time.time() - t0, ""
        except (KeyError, IndexError):
            return "", time.time() - t0, f"malformed: {str(resp)[:120]}"
    except (error.URLError, error.HTTPError, json.JSONDecodeError,
            ConnectionError, TimeoutError, OSError) as e:
        # OSError covers http.client.RemoteDisconnected + socket.timeout.
        # We swallow, return an empty text + error string, so the outer
        # loop can record the failure and continue with the next candidate
        # rather than abandoning the whole run.
        return "", time.time() - t0, str(e)[:200]


def ttt_one_prompt(prompt: str, n: int, gateway: str, channel: str = "imessage",
                   temperatures: list = None,
                   speaker_id_clf: dict = None,
                   persona_prompt: str = None) -> dict:
    """Generate n candidates for a prompt, score, pick best.

    The choice function:
      - If `speaker_id_clf` is available AND all candidates pass shape:
        argmax on P(Seth), shape-len tiebreak. This is the "shape is
        saturated, the speaker-ID classifier is the binding constraint"
        path. Discovered on 2026-05-19: when N=5 candidates all hit
        shape=1.0, the original shape+length tiebreaker silently picked
        AGAINST P(Seth) (mean Δ = -0.212).
      - Else fall back to argmax(shape, -len).

    Returns:
      {"prompt", "candidates": [{text, shape, p_seth, elapsed_s,
                                  temperature}],
       "chosen_idx", "chosen_text", "chosen_score", "chosen_p_seth",
       "choice_mode", "rejected_texts", "total_elapsed_s"}
    """
    if temperatures is None:
        # Spread temperatures to encourage candidate diversity
        temperatures = [0.7, 0.85, 0.95, 1.05, 1.15][:n]
        # Pad with mid-temperature if n > 5
        while len(temperatures) < n:
            temperatures.append(0.9)
    candidates = []
    t_start = time.time()
    for i in range(n):
        temp = temperatures[i]
        text, elapsed, err = generate(prompt, gateway, temperature=temp,
                                       persona_prompt=persona_prompt)
        shape = classify(text, channel=channel) if not err else {
            "pass": False, "score": 0.0, "len": 0, "fails": [f"gen-error: {err}"]}
        c_p_seth = None
        if speaker_id_clf is not None and text:
            from personaeval_speaker_id import p_seth as _p
            c_p_seth = _p(speaker_id_clf, text)
        candidates.append({
            "idx": i, "text": text, "shape": shape, "p_seth": c_p_seth,
            "elapsed_s": elapsed, "temperature": temp, "error": err or None,
        })

    # Choice function — see docstring for the rationale.
    all_shape_pass = (speaker_id_clf is not None and
                      all(c["shape"].get("score", 0) >= 1.0 and c["p_seth"] is not None
                          for c in candidates))
    if all_shape_pass:
        # Speaker-ID classifier is now the binding signal.
        scored = [(c["p_seth"], -c["shape"]["len"], c["idx"])
                  for c in candidates]
        choice_mode = "p_seth_argmax"
    else:
        # Fallback: shape primary, length tiebreak.
        scored = [(c["shape"]["score"], -c["shape"]["len"], c["idx"])
                  for c in candidates]
        choice_mode = "shape_argmax"
    scored.sort(reverse=True)
    chosen_idx = scored[0][2]
    chosen = candidates[chosen_idx]
    rejected = [c for c in candidates if c["idx"] != chosen_idx]

    return {
        "prompt": prompt,
        "candidates": candidates,
        "chosen_idx": chosen_idx,
        "chosen_text": chosen["text"],
        "chosen_score": chosen["shape"]["score"],
        "chosen_p_seth": chosen.get("p_seth"),
        "choice_mode": choice_mode,
        "rejected_texts": [r["text"] for r in rejected],
        "total_elapsed_s": time.time() - t_start,
    }


def log_to_dpo_pairs(db_path: str, results: list):
    """Log (prompt, chosen, rejected) triples to dpo_pairs for next ORPO round.
    Uses source = 'ttt_verifier' so downstream training can opt in/out.

    Schema: id, prompt, chosen, rejected, margin, timestamp, source.
    """
    con = sqlite3.connect(db_path)
    written = 0
    for r in results:
        if not r["chosen_text"]:
            continue  # skip pure-error rows
        for rej_text in r["rejected_texts"]:
            if not rej_text or rej_text == r["chosen_text"]:
                continue
            margin = 1.0  # shape_score gap, conservative default
            con.execute(
                "INSERT INTO dpo_pairs(prompt,chosen,rejected,margin,timestamp,source) "
                "VALUES(?,?,?,?,?,?)",
                (r["prompt"], r["chosen_text"], rej_text, margin, int(time.time()),
                 "ttt_verifier"),
            )
            written += 1
    con.commit()
    con.close()
    return written


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--prompt", help="Single prompt to TTT")
    p.add_argument("--batch", help="Path to eval suite JSON; runs TTT on each task")
    p.add_argument("--n", type=int, default=5, help="N candidates per prompt")
    p.add_argument("--gateway", default=GATEWAY_DEFAULT)
    p.add_argument("--channel", default="imessage")
    p.add_argument("--log-to-sqlite", help="Path to memory.db; logs DPO triples")
    p.add_argument("--out", help="Output JSON path", default="/tmp/ttt_results.json")
    p.add_argument("--speaker-id-clf", default=SPEAKER_ID_CLF_DEFAULT,
                   help="Path to trained speaker-ID classifier JSON. "
                        "When loaded AND all candidates pass shape, "
                        "choose by argmax P(Seth).")
    p.add_argument("--persona", action="store_true",
                   help="Inject the compact persona prompt as system "
                        "message. Required when --gateway points at MLX "
                        "direct (port 8741); the gateway path injects "
                        "persona automatically.")
    args = p.parse_args()
    speaker_id_clf = _maybe_load_speaker_id_clf(args.speaker_id_clf)
    if speaker_id_clf:
        print(f"speaker-ID classifier loaded from {args.speaker_id_clf} "
              f"(features={len(speaker_id_clf.get('feature_names', []))})")
    else:
        print(f"speaker-ID classifier NOT loaded (path={args.speaker_id_clf}); "
              f"using shape-only argmax")

    persona_prompt = None
    if args.persona:
        try:
            from memory_ablation import build_compact_persona_prompt
            persona_prompt = build_compact_persona_prompt()
            print(f"persona prompt loaded ({len(persona_prompt)} chars)")
        except (ImportError, FileNotFoundError) as e:
            print(f"WARNING: --persona requested but could not build prompt: {e}")

    prompts = []
    if args.prompt:
        prompts.append(args.prompt)
    if args.batch:
        suite = json.loads(Path(args.batch).read_text())
        for t in suite.get("tasks", []):
            if t.get("prompt"):
                prompts.append(t["prompt"])
    if not prompts:
        print("ERROR: provide --prompt or --batch", file=sys.stderr)
        sys.exit(2)

    print(f"TTT: {len(prompts)} prompt(s), N={args.n} candidates each\n")
    all_results = []
    for i, prompt in enumerate(prompts, 1):
        print(f"--- prompt {i}/{len(prompts)}: {prompt[:80]!r}")
        result = ttt_one_prompt(prompt, args.n, args.gateway, args.channel,
                                speaker_id_clf=speaker_id_clf,
                                persona_prompt=persona_prompt)
        all_results.append(result)
        p_seth_disp = (f", P(Seth)={result['chosen_p_seth']:.3f}"
                       if result.get("chosen_p_seth") is not None else "")
        print(f"    chosen (idx={result['chosen_idx']}, "
              f"score={result['chosen_score']:.2f}{p_seth_disp}, "
              f"mode={result.get('choice_mode', '?')}):")
        print(f"      {result['chosen_text']!r}")
        print(f"    rejected ({len(result['rejected_texts'])}):")
        for j, rej in enumerate(result["rejected_texts"][:3]):
            print(f"      [{j}] {rej[:80]!r}")
        print(f"    total elapsed: {result['total_elapsed_s']:.1f}s")
        print()

    # Aggregate
    chosen_scores = [r["chosen_score"] for r in all_results]
    all_first_scores = [r["candidates"][0]["shape"]["score"] for r in all_results]
    if chosen_scores:
        avg_chosen = sum(chosen_scores) / len(chosen_scores)
        avg_first = sum(all_first_scores) / len(all_first_scores)
        print(f"AGGREGATE: avg_chosen={avg_chosen:.3f}, avg_first_only={avg_first:.3f}")
        print(f"  Lift from TTT: +{avg_chosen - avg_first:.3f} per prompt")

    Path(args.out).write_text(json.dumps(all_results, indent=2))
    print(f"\nFull results: {args.out}")

    if args.log_to_sqlite:
        written = log_to_dpo_pairs(args.log_to_sqlite, all_results)
        print(f"Wrote {written} (prompt, chosen, rejected) triples to "
              f"dpo_pairs (source='ttt_verifier') for next ORPO round.")


if __name__ == "__main__":
    main()
