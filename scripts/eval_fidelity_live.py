#!/usr/bin/env python3
"""
Live-path persona-fidelity scoring (AC-11).

Unlike eval_fidelity_nightly.py (which scores base-vs-adapter OFFLINE via
mlx_lm.generate), this harness scores the path the ROUTER would actually pick
for production turns -- local Gemma+LoRA when the AUTO/FORCE policy + health say
so, else cloud -- and records `path_used` in the verdict JSON. That closes the
gap that made the offline +27pp number untrustworthy for production: here we
measure what the user actually talks to.

Generation modes:
  --path auto|local|cloud  Force a path, or let determine_path() decide.
  local  -> POST to the mlx_local server's OpenAI-compatible chat endpoint
            (the real serving path, exercising the adapter as loaded).
  cloud  -> requires the running daemon/provider creds; not generated here.
            Use --dry-run for a headless, deterministic harness run.
  --dry-run  Echo each fixture's reference as the "response" (no network), so
             CI/tests can exercise the harness end-to-end and assert the
             verdict-JSON shape without a live server.

Exit codes: 0 = verdict produced (PASS/SKIP), 1 = FAIL or harness error.

Usage:
  scripts/eval_fidelity_live.py --fixtures held_out.jsonl --path auto \\
      --mlx-url http://127.0.0.1:8741/v1 --output-json verdict.json
"""

import argparse
import json
import os
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from eval_fidelity_helpers import (  # noqa: E402
    compute_persona_fidelity_scores,
    load_held_out_prompts_from_jsonl,
)

# Fidelity gate: mean shape score below this is a FAIL on the live path.
LIVE_FIDELITY_FLOOR = 0.5


def determine_path(routing_mode: str, adapter_present: bool, server_reachable: bool) -> str:
    """Mirror the C router/health policy (model_router.c + model_router_health.c).

    OFF   -> always cloud.
    FORCE -> local (the turn-level fallback handles a downed server at runtime).
    AUTO  -> local only when an adapter exists AND the server is reachable.
    Any other / unknown mode is treated as AUTO.
    """
    mode = (routing_mode or "auto").lower()
    if mode == "off":
        return "cloud"
    if mode == "force":
        return "local"
    # auto (default)
    return "local" if (adapter_present and server_reachable) else "cloud"


def mlx_server_reachable(mlx_url: str, timeout: float = 1.5) -> bool:
    if not mlx_url:
        return False
    try:
        base = mlx_url.rstrip("/")
        req = urllib.request.Request(base + "/models", method="GET")
        with urllib.request.urlopen(req, timeout=timeout) as resp:  # noqa: S310 (localhost)
            return 200 <= resp.status < 500
    except Exception:
        return False


def generate_local(mlx_url: str, prompt: str, max_tokens: int = 80, timeout: float = 30.0) -> str:
    """Generate via the real mlx_local serving path (OpenAI-compatible chat)."""
    base = mlx_url.rstrip("/")
    body = json.dumps(
        {
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": max_tokens,
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        base + "/chat/completions", data=body, headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:  # noqa: S310 (localhost)
        payload = json.loads(resp.read().decode("utf-8"))
    return payload["choices"][0]["message"]["content"]


def score_live_path(
    prompts: list,
    path_used: str,
    mlx_url: str,
    dry_run: bool,
    channel: str = "imessage",
) -> dict:
    """Generate responses on the chosen path, score them, return a verdict dict."""
    responses: list[str] = []
    errors = 0
    for p in prompts:
        prompt_text = p.get("prompt") or p.get("instruction") or ""
        if dry_run:
            # Deterministic, network-free: the reference IS the response.
            responses.append(p.get("reference") or p.get("response") or "")
            continue
        if path_used == "local":
            try:
                responses.append(generate_local(mlx_url, prompt_text))
            except Exception as e:  # noqa: BLE001 -- surface, don't crash the harness
                errors += 1
                responses.append("")
                print(f"[warn] local generation failed: {e}", file=sys.stderr)
        else:
            # Cloud generation requires the daemon/provider credentials; this
            # harness does not call cloud providers directly. Use --dry-run for
            # a headless run, or score cloud output captured elsewhere.
            return {
                "verdict": "SKIP",
                "path_used": path_used,
                "reason": "cloud generation not supported in standalone harness; "
                "use --dry-run or run against the daemon",
            }

    _classifications, mean_score = compute_persona_fidelity_scores(responses, channel=channel)
    verdict = "FAIL" if mean_score < LIVE_FIDELITY_FLOOR else "PASS"
    return {
        "verdict": verdict,
        "path_used": path_used,
        "channel": channel,
        "n": len(responses),
        "errors": errors,
        "mean_score": round(float(mean_score), 4),
        "floor": LIVE_FIDELITY_FLOOR,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Live-path persona-fidelity scoring (AC-11)")
    ap.add_argument("--fixtures", type=Path, required=True, help="held-out prompts JSONL")
    ap.add_argument("--path", choices=["auto", "local", "cloud"], default="auto")
    ap.add_argument(
        "--routing-mode", default="auto", help="config mlx_local_routing (off/auto/force)"
    )
    ap.add_argument(
        "--mlx-url", default=os.environ.get("HUMAN_MLX_URL", "http://127.0.0.1:8741/v1")
    )
    ap.add_argument("--adapter-path", type=Path, default=None)
    ap.add_argument("--channel", default="imessage")
    ap.add_argument("--dry-run", action="store_true", help="echo references; no network")
    ap.add_argument("--output-json", type=Path, default=None)
    args = ap.parse_args()

    if not args.fixtures.exists():
        print(f"[error] fixtures not found: {args.fixtures}", file=sys.stderr)
        return 1
    prompts = load_held_out_prompts_from_jsonl(str(args.fixtures))

    if args.path == "auto":
        adapter_present = bool(args.adapter_path and args.adapter_path.exists())
        server_reachable = args.dry_run or mlx_server_reachable(args.mlx_url)
        path_used = determine_path(args.routing_mode, adapter_present, server_reachable)
    else:
        path_used = args.path

    verdict = score_live_path(prompts, path_used, args.mlx_url, args.dry_run, channel=args.channel)

    text = json.dumps(verdict, indent=2)
    print(text)
    if args.output_json:
        args.output_json.write_text(text)

    return 1 if verdict.get("verdict") == "FAIL" else 0


if __name__ == "__main__":
    sys.exit(main())
