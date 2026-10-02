#!/usr/bin/env python3
# scripts/eval_empty_reply_rate.py
#
# Offline empty-reply rate for a candidate persona adapter, measured against a
# SPARE mlx-server instance -- never production. Sends a fixed prompt set
# (classifier-style + chat, scripts/eval_data/empty_reply_prompts.jsonl) N times
# each to http://127.0.0.1:<port>/v1/chat/completions and counts replies whose
# visible text is empty after stripping.
#
# Why: the served seth-glm-air-mlxtune-orpo-20260905-* adapter, trained without
# the production chat template or an end-of-turn token, returned an empty reply
# on ~13% of classifier-style requests, which pushed traffic to cloud failover.
# A retrained adapter must show a lower rate than the serving adapter, on the
# same prompts, on the same spare server config, before it goes to the blind
# A/B gate. See docs/guides/persona-adapter-retrain-runbook.md.
#
# EMPTY-RETRY MUST BE OFF (2026-10-01, gemma-realtime c02bd50/32cd6d1): the
# server now regenerates an empty adapter reply on base weights by default
# (MLX_EMPTY_RETRY, default ON), which hides exactly the failure this measures
# -- both arms would read ~0%. /health does not expose the switch, so the
# ruling is: (1) PRECONDITION -- read the listening process's own environment
# (`lsof` for the pid, `ps eww` for its env) and refuse unless
# MLX_EMPTY_RETRY is explicitly 0/false/no/off; the server reads os.environ
# and never sets it, so the exec-time env is the switch's value. If the env
# cannot be read, refuse unless --retry-disabled-confirmed AND --server-log
# are both given. (2) TRIPWIRE -- with --server-log, any new `[empty-retry]`
# line during the run voids it (the server logs one per retry event).
#
# "empty" here is a SUPERSET of the server's _is_empty_generation: any reply
# whose visible text is empty after stripping scaffold counts, including long
# generations a server guard emptied (runaway deliberation, echo), which the
# server's retry deliberately ignores. For a candidate-vs-serving comparison
# on the same server both arms use the same definition.
#
# Refuses ports 8741 (production) and 8743 (arena). Never starts or stops a
# server. Exit codes: 0 measured (and under --max-empty-rate if given),
# 1 measured but over --max-empty-rate, 2 NOT measured (server unreachable,
# any request error, or the server's loaded adapter differs from --expect-adapter).
# Request errors are never counted as empty replies, and a run with errors
# writes no result file -- see .claude/rules/no-number-without-a-measurement.md.
"""Empty-reply rate of an adapter served on a spare mlx-server port."""

import argparse
import json
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

FORBIDDEN_PORTS = {8741: "production", 8743: "conversation-arena"}
DEFAULT_PROMPTS = Path(__file__).parent / "eval_data" / "empty_reply_prompts.jsonl"
# Scaffold that can survive server-side stripping; a reply consisting only of
# these is empty for every caller downstream.
_SCAFFOLD_RE = re.compile(r"</?think>|<\|(?:user|assistant|system|observation|endoftext)\|>|/nothink")


def check_port(port: int) -> None:
    if port in FORBIDDEN_PORTS:
        raise SystemExit(f"REFUSING port {port} ({FORBIDDEN_PORTS[port]}): evaluate on a spare "
                         "server instance, never a live one")


_RETRY_ENV_RE = re.compile(r"(?:^|\s)MLX_EMPTY_RETRY=(\S*)")
_RETRY_OFF = ("0", "false", "no", "off")
RETRY_LOG_MARK = "[empty-retry]"


def empty_retry_state(env_cmdline):
    """'off' | 'on' from a `ps eww` line. Mirrors mlx-server.py
    _empty_retry_enabled(): unset or any value but 0/false/no/off is ON."""
    m = _RETRY_ENV_RE.search(env_cmdline or "")
    if m and m.group(1).strip().lower() in _RETRY_OFF:
        return "off"
    return "on"


def server_env(port):
    """`ps eww` line (command + exec-time environment) of the process
    listening on 127.0.0.1:<port>, or None if it cannot be read."""
    try:
        pids = subprocess.run(["lsof", "-tnP", f"-iTCP:{port}", "-sTCP:LISTEN"],
                              capture_output=True, text=True, timeout=10).stdout.split()
        if len(pids) != 1:
            return None
        out = subprocess.run(["ps", "eww", "-o", "command=", "-p", pids[0]],
                             capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    # ps prints the env only for processes it may inspect; without it there is
    # no '=' assignment after the argv and the state is unknowable.
    return out if "mlx-server" in out and "=" in out else None


def retry_lines(log_path):
    if not log_path or not Path(log_path).is_file():
        return None
    return Path(log_path).read_text(errors="replace").count(RETRY_LOG_MARK)


def visible_text(content) -> str:
    if not isinstance(content, str):
        return ""
    return _SCAFFOLD_RE.sub("", content).strip()


def is_empty_reply(content) -> bool:
    return visible_text(content) == ""


def load_prompts(path):
    rows = []
    for line in Path(path).read_text().splitlines():
        if line.strip():
            r = json.loads(line)
            if r.get("category") not in ("classifier", "chat") or not r.get("messages"):
                raise SystemExit(f"bad prompt row in {path}: {line[:120]}")
            rows.append(r)
    if not rows:
        raise SystemExit(f"no prompts in {path}")
    return rows


def _http(url, body=None, timeout=180):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, headers={
        "Content-Type": "application/json", "X-HU-Priority": "batch"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def summarize(results):
    """{category: {n, empty, rate}} plus 'all'. n counts completed requests only."""
    out = {}
    for cat in ("classifier", "chat", "all"):
        sel = [r for r in results if cat == "all" or r["category"] == cat]
        n = len(sel)
        empty = sum(1 for r in sel if r["empty"])
        out[cat] = {"n": n, "empty": empty, "rate": (empty / n) if n else None}
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, required=True, help="spare server port (not 8741/8743)")
    ap.add_argument("--prompts", default=str(DEFAULT_PROMPTS))
    ap.add_argument("--samples", type=int, default=3, help="requests per prompt")
    ap.add_argument("--max-tokens", type=int, default=200)
    ap.add_argument("--temperature", type=float, default=0.7)
    ap.add_argument("--label", required=True, help="e.g. serving-20260905 or candidate-<stamp>")
    ap.add_argument("--expect-adapter", default=None,
                    help="refuse unless /health reports this adapter path as applied")
    ap.add_argument("--out", required=True, help="result JSON path")
    ap.add_argument("--max-empty-rate", type=float, default=None,
                    help="exit 1 if the classifier empty rate exceeds this")
    ap.add_argument("--server-log", default=None,
                    help="the spare server's stdout log; any new [empty-retry] line voids the run")
    ap.add_argument("--retry-disabled-confirmed", action="store_true",
                    help="only if the server env is unreadable: you launched it with "
                         "MLX_EMPTY_RETRY=0 (requires --server-log)")
    args = ap.parse_args(argv)

    check_port(args.port)
    env = server_env(args.port)
    if env is not None:
        retry = empty_retry_state(env)
        if retry != "off":
            print(f"NOT MEASURED: the server on :{args.port} runs with MLX_EMPTY_RETRY on "
                  "(default) -- empty adapter replies are regenerated on base weights and "
                  "would read as 0%. Relaunch it with MLX_EMPTY_RETRY=0.", file=sys.stderr)
            return 2
        retry_basis = "server process environment: MLX_EMPTY_RETRY off"
    elif args.retry_disabled_confirmed and retry_lines(args.server_log) is not None:
        retry_basis = "operator-confirmed MLX_EMPTY_RETRY=0 + server-log tripwire"
    else:
        print(f"NOT MEASURED: cannot read the environment of the server on :{args.port} to "
              "confirm MLX_EMPTY_RETRY=0. Pass --retry-disabled-confirmed together with "
              "--server-log <its stdout log> if you launched it with MLX_EMPTY_RETRY=0.",
              file=sys.stderr)
        return 2
    retry_before = retry_lines(args.server_log)
    base = f"http://127.0.0.1:{args.port}"
    try:
        health = _http(f"{base}/health", timeout=10)
    except (urllib.error.URLError, OSError, ValueError) as e:
        print(f"NOT MEASURED: {base}/health unreachable: {e}", file=sys.stderr)
        return 2
    adapter = health.get("active_adapter") or health.get("adapter")
    if args.expect_adapter is not None:
        want = str(Path(args.expect_adapter).expanduser())
        if adapter != want or not health.get("adapter_applied"):
            print(f"NOT MEASURED: server adapter {adapter!r} (applied="
                  f"{health.get('adapter_applied')}) != expected {want!r}", file=sys.stderr)
            return 2

    prompts = load_prompts(args.prompts)
    results, errors = [], []
    for p in prompts:
        for k in range(args.samples):
            body = {"messages": p["messages"], "max_tokens": args.max_tokens,
                    "temperature": args.temperature}
            t0 = time.time()
            try:
                resp = _http(f"{base}/v1/chat/completions", body)
                choice = (resp.get("choices") or [{}])[0]
                content = (choice.get("message") or {}).get("content")
            except (urllib.error.URLError, OSError, ValueError, KeyError) as e:
                errors.append({"id": p["id"], "sample": k, "error": str(e)})
                continue
            results.append({"id": p["id"], "category": p["category"], "sample": k,
                            "empty": is_empty_reply(content),
                            "finish_reason": choice.get("finish_reason"),
                            "reply": visible_text(content)[:200],
                            "latency_s": round(time.time() - t0, 2)})

    if errors:
        print(f"NOT MEASURED: {len(errors)} request error(s), e.g. {errors[0]} -- "
              "no result written", file=sys.stderr)
        return 2

    retry_after = retry_lines(args.server_log)
    if retry_before is not None and retry_after != retry_before:
        print(f"NOT MEASURED: {args.server_log} gained {RETRY_LOG_MARK} lines during the run -- "
              "the server retried empty replies, so they were not observed. No result written.",
              file=sys.stderr)
        return 2

    summary = summarize(results)
    report = {"label": args.label, "port": args.port, "model": health.get("model"),
              "adapter": adapter, "adapter_applied": health.get("adapter_applied"),
              "tensors_loaded": health.get("tensors_loaded"),
              "prompts_file": str(args.prompts), "samples": args.samples,
              "max_tokens": args.max_tokens, "temperature": args.temperature,
              "empty_retry_off_basis": retry_basis,
              "server_log_tripwire": args.server_log,
              "empty_definition": "visible text empty after stripping scaffold; a SUPERSET of "
                                  "mlx-server _is_empty_generation (also counts long "
                                  "generations a server guard emptied)",
              "summary": summary, "results": results}
    Path(args.out).write_text(json.dumps(report, indent=2) + "\n")
    for cat in ("classifier", "chat", "all"):
        s = summary[cat]
        print(f"{args.label} {cat:10s} empty {s['empty']}/{s['n']} = {s['rate']:.1%}")
    print(f"wrote {args.out}")
    rate = summary["classifier"]["rate"]
    if args.max_empty_rate is not None and rate is not None and rate > args.max_empty_rate:
        print(f"FAIL: classifier empty rate {rate:.1%} > {args.max_empty_rate:.1%}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
