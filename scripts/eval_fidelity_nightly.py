#!/usr/bin/env python3
"""
Nightly fidelity eval harness + SOTA gate for M3 continuous learning.

Loads 20-30 held-out iMessage prompts (real Seth conversation history,
date-stratified to avoid training contamination), runs two-pass inference
(base model alone, then base + persona-fidelity adapter), scores each
response using the deterministic shape classifier, computes bootstrap CI,
and gates on both statistical (one-sided t-test α=0.025) and practical
(delta ≥ 5%) significance.

Generation runs THROUGH the production mlx-server when one is up (--gen
served, the auto default): PRE swaps a zero-delta twin of the serving adapter
in via POST /v1/adapters/swap (LoRA is additive, so all-zero A/B is the base
model exactly), POST runs on the serving adapter after it is restored and
verified via /v1/adapters/current. Never two loaders: on 2026-09-03 the
in-process path loaded a second 56 GB copy beside :8741, wired memory hit
94.8 GB and the server died ("[METAL] Insufficient Memory"). In-process
generation (mlx_lm.load once per pass) is only allowed when no server is up;
the per-prompt subprocess shape survives behind --subprocess-gen.

Verdict logged to stdout and JSON, suitable for launchd scheduling.

Usage:
  # Preferred: no --adapter-path — the SERVING adapter is resolved dynamically
  # (live mlx-server process, else config.json personalization.lora_adapter_path)
  python3 scripts/eval_fidelity_nightly.py \\
    --model-id mlx-community/gemma-4-31b-it-4bit \\
    --output-json ~/.human/logs/eval-fidelity-nightly.json

  # Explicit override (pin a specific adapter under test)
  python3 scripts/eval_fidelity_nightly.py \\
    --adapter-path ~/.human/training-data/adapters/seth-lora-v5-8bit-20260718-105251

Exit codes:
  0 = PASS (gate executed, adapter measurably better)
  1 = FAIL (adapter measurably worse, or gate missing components)
  2 = DEFERRED (no measurement happened: mlx_lm or model unavailable, too few
      valid pairs, a cross-family base/adapter pair, or PRE and POST produced
      byte-identical output — see responses_identical)
  3 = SKIP (no adapter / no prompts / no measurable delta) — deliberately
      non-zero and paired with a greppable FIDELITY_SKIP stdout marker so a
      silent skip can never masquerade as a healthy nightly again (a stale
      hardcoded adapter path skipped silently for 13 nights in 2026-07).
"""

import argparse
import gc
import json
import os
import re
import shlex
import shutil
import signal
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from contextlib import contextmanager
from datetime import datetime
from pathlib import Path

# Reuse shared utilities
import sys
sys.path.insert(0, str(Path(__file__).parent))
from eval_fidelity_helpers import (
    DEFAULT_SPEAKER_MODEL_PATH,
    SHAPE_WEIGHT,
    SPEAKER_WEIGHT,
    bootstrap_ci,
    compute_persona_fidelity_scores,
    load_held_out_prompts_from_jsonl,
    load_speaker_model,
)  # noqa: E402
import adapter_registry

# Defaults
DEFAULT_FIXTURE = Path(__file__).parent.parent / "docs/plans/2026-05-26-sprint-56-gemma-as-seth/data/heldout-prompts.jsonl"
DEFAULT_LOG_DIR = Path.home() / ".human" / "logs"
DEFAULT_MODEL = "mlx-community/gemma-4-31b-it-4bit"
DEFAULT_CONFIG_PATH = Path.home() / ".human" / "config.json"

# Exit codes (see module docstring)
EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_DEFERRED = 2
EXIT_SKIP = 3

# SOTA gate thresholds (per design US-9, AC-9.5 and AC-9.6)
ALPHA_ONESIDED = 0.025  # one-sided t-test significance level
CONFIDENCE = 1 - 2 * ALPHA_ONESIDED  # 0.95 for two-sided, 0.975 for one-sided
PRACTICAL_DELTA_FLOOR = 0.05  # 5% absolute minimum improvement


def production_mlx_port(env: dict | None = None) -> str:
    """Port of the PRODUCTION mlx-server — the one the nightly must eval.
    Mirrors lora_training_runner.c resolve_mlx_base_url():
    HU_MLX_BASE_URL env override, else 8741.

    This filter matters: multiple mlx-servers run concurrently (observed
    2026-07-26: gemma-8bit realtime on :8747 alongside production GLM on
    :8741), and `ps` order is arbitrary — first-match would eval whichever
    server happened to be listed first.
    """
    env = os.environ if env is None else env
    url = env.get("HU_MLX_BASE_URL", "")
    m = re.search(r"//[^/]*:(\d+)", url)
    return m.group(1) if m else "8741"


def _iter_production_mlx_server_tokens(ps_output: str, port: str):
    """Yield shlex-token lists for mlx-server processes on `port` only.
    A process with no --port flag is assumed to be on the default 8741."""
    for line in ps_output.splitlines():
        if "mlx-server" not in line:
            continue
        try:
            tokens = shlex.split(line)
        except ValueError:
            continue
        line_port = "8741"
        if "--port" in tokens:
            try:
                line_port = tokens[tokens.index("--port") + 1]
            except IndexError:
                continue
        if line_port == port:
            yield tokens


def resolve_serving_adapter(
    ps_output: str | None = None,
    config_path: Path = DEFAULT_CONFIG_PATH,
) -> tuple[Path | None, str]:
    """Resolve the adapter that is ACTUALLY serving, not a hardcoded name.

    Priority (each candidate must exist on disk to win):
      1. The PRODUCTION mlx-server process's --adapter-path argument — ground
         truth for what is serving right now. Only processes on the
         production port count (see production_mlx_port).
      2. config.json personalization.lora_adapter_path — what will serve
         after the next restart.

    Args:
        ps_output: process listing to scan (injectable for tests);
                   None = run `ps ax -o command` here
        config_path: path to ~/.human/config.json (injectable for tests)

    Returns:
        (adapter_path, source_description); (None, reason) when unresolvable.
    """
    if ps_output is None:
        try:
            ps_output = subprocess.run(
                ["ps", "ax", "-o", "command"],
                capture_output=True, text=True, timeout=10,
            ).stdout
        except Exception:
            ps_output = ""

    port = production_mlx_port()
    for tokens in _iter_production_mlx_server_tokens(ps_output, port):
        try:
            candidate = Path(tokens[tokens.index("--adapter-path") + 1])
        except (ValueError, IndexError):
            continue
        if candidate.exists():
            return (candidate,
                    f"live mlx-server process --adapter-path (port {port})")

    try:
        config = json.loads(Path(config_path).read_text())
        raw = config.get("personalization", {}).get("lora_adapter_path")
        if raw:
            candidate = Path(raw).expanduser()
            if candidate.exists():
                return (candidate, "config.json personalization.lora_adapter_path")
    except (OSError, json.JSONDecodeError):
        pass

    return (None, "no live mlx-server and no usable personalization.lora_adapter_path")


def resolve_serving_model(ps_output: str | None = None) -> str:
    """Resolve the SERVING base model from the live mlx-server process.

    The adapter's delta is only meaningful against the base it was trained on
    (e.g. seth-lora-v5-8bit belongs to the 8bit base). Before 2026-07-25 the
    nightly hardcoded the 4bit base while production served 8bit. Only
    processes on the production port count (see production_mlx_port) — a
    spare mlx-server on another port must not win by ps ordering.
    Falls back to DEFAULT_MODEL when no production server is running.
    """
    if ps_output is None:
        try:
            ps_output = subprocess.run(
                ["ps", "ax", "-o", "command"],
                capture_output=True, text=True, timeout=10,
            ).stdout
        except Exception:
            ps_output = ""

    port = production_mlx_port()
    for tokens in _iter_production_mlx_server_tokens(ps_output, port):
        try:
            return tokens[tokens.index("--model") + 1]
        except (ValueError, IndexError):
            continue

    # Server down (3am nightly races the restart window) — fall back to CONFIG,
    # not to the hardcoded constant. The adapter half of this resolution already
    # falls back to config.json (personalization.lora_adapter_path); before
    # 2026-07-27 the base half did not, so a server-down night silently paired
    # the config's GLM adapter with the hardcoded gemma base. A cross-family
    # LoRA cannot bind, so it was a no-op: pre == post == 0.3178 exactly, delta
    # 0, recorded as a legitimate-looking SKIP. Config is the same source the
    # daemon serves from, so it is right whenever the process probe is merely absent.
    try:
        config = json.loads(DEFAULT_CONFIG_PATH.read_text())
        model = config.get("mlx_local", {}).get("model")
        if isinstance(model, str) and model.strip():
            return model.strip()
    except (OSError, json.JSONDecodeError):
        pass
    return DEFAULT_MODEL


def model_family(name: str | None) -> str | None:
    """Coarse model-family tag ('glm' / 'gemma') from a model id or adapter path.

    Deliberately conservative: returns None when no known family marker is
    present, so unknown naming never fabricates a mismatch."""
    if not name:
        return None
    lowered = str(name).lower()
    for family in ("glm", "gemma"):
        if family in lowered:
            return family
    return None


def adapter_declared_base(adapter_path) -> str | None:
    """The base model an adapter records it was trained against, or None.

    mlx_lm writes the training base into adapter_config.json['model']. That is
    EXACT provenance where model_family() is only a substring heuristic — it
    also separates quantizations of one model, which the family tag cannot.

    Reported as evidence and warned on, never gated: an adapter trained on one
    quantization of a base can legitimately bind to another, so a declared-base
    difference is not by itself proof of a fault. Only the OBSERVED no-op
    (responses_identical) is allowed to DEFER.
    """
    try:
        config = json.loads((Path(adapter_path) / "adapter_config.json").read_text())
    except (OSError, json.JSONDecodeError, TypeError):
        return None
    declared = config.get("model")
    return declared.strip() if isinstance(declared, str) and declared.strip() else None


def responses_identical(pre: list[str], post: list[str]) -> bool:
    """True when the two passes produced byte-identical output on EVERY prompt.

    This is the general no-op detector, and it catches a class the family guard
    structurally cannot. mlx_lm's load_adapters() ends in

        model.load_weights(adapter_path / "adapters.safetensors", strict=False)

    so an adapter whose tensor keys do not match the base loads ZERO weights —
    no error, no warning. LoRALinear initialises `lora_b` to zeros, so the LoRA
    branch computes `(x @ lora_a) @ 0 == 0` and the "adapted" model is
    bit-identical to base. Wrong base family, wrong quantization, renamed
    modules, a truncated adapter file: all land here, all silently.

    At temp 0.0 a genuinely bound LoRA perturbs the logits, so across ~29
    prompts at least one greedy decode diverges. Total identity across every
    prompt means no delta was applied at all — evidence about the plumbing,
    never about the adapter.
    """
    return bool(pre) and len(pre) == len(post) and all(a == b for a, b in zip(pre, post))


def base_adapter_family_mismatch(model_id: str | None, adapter_path) -> bool:
    """True when base and adapter are confidently from DIFFERENT families.

    A cross-family LoRA does not bind — it applies no delta at all, so the run
    scores pre == post and reports a SKIP that reads as 'the adapter didn't
    help' when the truth is 'the adapter was never applied'. That is a
    measurement fault, not a verdict, and must DEFER (2026-07-27 nightly)."""
    base = model_family(model_id)
    adapter = model_family(adapter_path)
    return bool(base and adapter and base != adapter)


# Error markers generate() can return in place of model output. These MUST be
# excluded from scoring: '[timeout]' is short and casual-looking, so the shape
# classifier scores it 1.0 — for ~10 nights in 2026-07 every call hit the old
# 180s timeout and pure sentinel passes recorded as pre=post=1.0 SKIP.
SENTINEL_PREFIXES = ("[timeout]", "[gen_err", "[empty]")


def is_sentinel(response: str) -> bool:
    """True when a response is a generate() error marker, not model output."""
    return response.startswith(SENTINEL_PREFIXES)


def emit_skip(reason: str, output_json: Path | None) -> int:
    """Log a SKIP loudly (greppable FIDELITY_SKIP marker), write verdict, exit 3."""
    verdict = {
        "timestamp": datetime.now().isoformat(),
        "verdict": "SKIP",
        "reason": reason,
        "exit_code": EXIT_SKIP,
    }
    print(f"[SKIP] FIDELITY_SKIP {reason}", flush=True)
    if output_json:
        output_json.write_text(json.dumps(verdict, indent=2))
    return EXIT_SKIP


def emit_deferred(reason: str, output_json: Path | None, **extra) -> int:
    """Log a DEFERRED loudly (greppable FIDELITY_DEFERRED marker), write it, exit 2.

    DEFERRED, not SKIP: a deferral says "no measurement happened", where SKIP
    says "the adapter measurably did not help". Emitting the latter for the
    former is how a plumbing fault gets attributed to the model.
    """
    verdict = {
        "timestamp": datetime.now().isoformat(),
        "verdict": "DEFERRED",
        "reason": reason,
        "exit_code": EXIT_DEFERRED,
        **extra,
    }
    print(f"[DEFERRED] FIDELITY_DEFERRED {reason}", flush=True)
    if output_json:
        output_json.write_text(json.dumps(verdict, indent=2))
    return EXIT_DEFERRED


class GenerationTimeout(Exception):
    """Raised inside wall_clock_guard when the per-call deadline passes."""


@contextmanager
def wall_clock_guard(seconds: int):
    """Wall-clock deadline for in-process calls, via SIGALRM.

    Replaces the subprocess kill deadline for the in-process path. SIGALRM
    (not a thread/executor timeout) because an abandoned generation thread
    would keep the Metal GPU busy and serialize behind every later call;
    the alarm raises INSIDE the mlx_lm token loop, actually aborting it.
    Granularity caveat: signals fire between Python bytecodes, so a single
    long Metal kernel isn't preempted mid-op — the token loop returns to
    Python every token, which is granular enough here.

    Only usable in the main thread (signal module restriction); elsewhere
    it degrades to no guard rather than raising.
    """
    if threading.current_thread() is not threading.main_thread():
        yield
        return

    def _on_alarm(signum, frame):
        raise GenerationTimeout(f"exceeded {seconds}s wall clock")

    prev = signal.signal(signal.SIGALRM, _on_alarm)
    signal.alarm(max(1, int(seconds)))
    try:
        yield
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, prev)


# Model load gets its own (generous) deadline: cold-loading the 31B weights
# from disk under GPU/IO contention can take minutes and is expected; a HUNG
# load should still turn into a DEFERRED verdict instead of an eternal night.
LOAD_TIMEOUT_SEC = 900


def load_model(model_id: str, adapter_path: str | None = None,
               timeout_sec: int = LOAD_TIMEOUT_SEC):
    """Load model + tokenizer in-process, ONCE per pass.

    PRE pass calls this with adapter_path=None (base model alone); POST pass
    with the resolved adapter. Raises on failure — main() maps that to
    DEFERRED (exit 2), matching the old "mlx_lm or model unavailable" contract.
    Import is lazy so tests and the --subprocess-gen path never import mlx.
    """
    from mlx_lm import load  # noqa: PLC0415 — lazy by design
    with wall_clock_guard(timeout_sec):
        return load(str(model_id),
                    adapter_path=str(adapter_path) if adapter_path else None)


def free_model() -> None:
    """Release model memory between passes (caller drops its refs first).

    The subprocess path got this for free via process exit; in-process the
    PRE model must be freed before the POST load or two 31B copies coexist.
    """
    gc.collect()
    try:
        import mlx.core as mx  # noqa: PLC0415 — lazy by design
        clear = getattr(mx, "clear_cache", None) or mx.metal.clear_cache
        clear()
    except Exception:
        pass  # freeing is best-effort; gc alone still drops the weights


def _mlx_generate(model, tokenizer, prompt: str, max_tokens: int) -> str:
    """One in-process generation. Thin seam: tests patch THIS symbol.

    Mirrors the mlx_lm CLI behavior the subprocess path relied on: apply the
    tokenizer's chat template when it has one, and sample at temp 0.0
    (deterministic, same as the old `--temp 0.0`).
    """
    from mlx_lm import generate as mlx_generate  # noqa: PLC0415
    from mlx_lm.sample_utils import make_sampler  # noqa: PLC0415

    prompt_input = prompt
    if getattr(tokenizer, "chat_template", None):
        prompt_input = tokenizer.apply_chat_template(
            [{"role": "user", "content": prompt}], add_generation_prompt=True
        )
    return mlx_generate(model, tokenizer, prompt=prompt_input,
                        max_tokens=max_tokens, sampler=make_sampler(temp=0.0))


def generate_inprocess(model, tokenizer, prompt: str,
                       max_tokens: int = 80, timeout_sec: int = 600) -> str:
    """Generate one response on an already-loaded model.

    Same return contract as the subprocess generate(): model text, or one of
    the SENTINEL_PREFIXES markers ('[timeout]' / '[gen_err: ...]' / '[empty]')
    that callers filter via is_sentinel(). Never raises for per-prompt
    failures — a broken PASS (load failure) raises from load_model instead.
    """
    try:
        with wall_clock_guard(timeout_sec):
            text = _mlx_generate(model, tokenizer, prompt, max_tokens)
    except GenerationTimeout:
        return "[timeout]"
    except Exception as e:
        return f"[gen_err: {str(e)[:100]}]"

    text = (text or "").strip()
    return text[-300:].strip() if text else "[empty]"


def generate(model_id: str, prompt: str, adapter_path: str | None = None,
             max_tokens: int = 80, timeout_sec: int = 600) -> str:
    """Invoke mlx_lm.generate via subprocess (legacy --subprocess-gen path).

    Cold-loads the model EVERY call — 29 prompts x 2 passes = 58 cold loads
    per night, ~3h wall time. Kept only as an operational fallback behind
    --subprocess-gen; the default path is generate_inprocess() on a model
    loaded once per pass.

    Args:
        model_id: HuggingFace model identifier
        prompt: input text
        adapter_path: optional LoRA adapter path
        max_tokens: max generation tokens (default 80 per design)
        timeout_sec: subprocess kill deadline. Each call cold-loads the 31B
            model, which takes >180s under GPU contention with the resident
            server — the old 180s default timed out EVERY call for ~10 nights.

    Returns:
        Generated response string, or error marker if subprocess fails
    """
    cmd = [
        sys.executable, "-m", "mlx_lm", "generate",
        "--model", model_id,
        "--prompt", prompt,
        "--max-tokens", str(max_tokens),
        "--temp", "0.0",  # deterministic
    ]
    if adapter_path:
        cmd.extend(["--adapter-path", str(adapter_path)])

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        return "[timeout]"
    except Exception as e:
        return f"[gen_err: {str(e)[:100]}]"

    if result.returncode != 0:
        stderr_snippet = result.stderr[-150:].strip() if result.stderr else "(no stderr)"
        return f"[gen_err: {stderr_snippet}]"

    # mlx_lm.generate prints framed output with prompts, tokens/sec, etc.
    # Strip the metadata and keep only the actual response.
    out = result.stdout
    lines = [
        l for l in out.splitlines()
        if l and not l.startswith("==")
        and not l.startswith("Prompt")
        and not l.startswith("Generation:")
        and "tokens-per-sec" not in l
        and "Peak memory" not in l
    ]
    response = ("\n".join(lines)[-300:]).strip()
    return response if response else "[empty]"


def run_eval_pass(model_id: str, prompts: list[dict], adapter_path: str | None = None,
                  gen_timeout: int = 600,
                  use_subprocess: bool = False) -> tuple[list[str], dict]:
    """Run one pass of generation (pre or post adapter).

    Default: load the model ONCE via the mlx_lm Python API, generate every
    prompt in-process on the warm model, then free it. The old shape (one
    `python3 -m mlx_lm generate` subprocess per prompt = 58 cold loads of the
    31B model per night, ~3h wall) survives behind use_subprocess only.

    Args:
        model_id: HuggingFace model
        prompts: list of prompt dicts with "prompt" field
        adapter_path: optional LoRA adapter path (None = PRE/base pass)
        gen_timeout: per-prompt wall-clock guard seconds (in-process), or
            per-subprocess kill deadline (use_subprocess)
        use_subprocess: legacy per-prompt subprocess path (--subprocess-gen)

    Returns:
        (responses, stats) where stats includes pass label and timing
    """
    start = time.time()
    responses = []
    pass_label = "POST (adapter)" if adapter_path else "PRE (base)"

    if use_subprocess:
        for i, p in enumerate(prompts):
            prompt_text = p["prompt"] if isinstance(p, dict) else p
            print(f"  [{pass_label}] {i+1}/{len(prompts)} {prompt_text[:50]!r}...", flush=True)
            responses.append(generate(model_id, prompt_text, adapter_path=adapter_path,
                                      timeout_sec=gen_timeout))
        elapsed = time.time() - start
        return (responses, {"pass": pass_label, "elapsed_sec": elapsed,
                            "count": len(responses)})

    print(f"  [{pass_label}] loading {model_id} "
          f"(adapter={adapter_path or 'none'})...", flush=True)
    load_start = time.time()
    model, tokenizer = load_model(model_id, adapter_path=adapter_path)
    load_sec = time.time() - load_start
    print(f"  [{pass_label}] model loaded in {load_sec:.1f}s", flush=True)

    try:
        for i, p in enumerate(prompts):
            prompt_text = p["prompt"] if isinstance(p, dict) else p
            print(f"  [{pass_label}] {i+1}/{len(prompts)} {prompt_text[:50]!r}...", flush=True)
            responses.append(generate_inprocess(model, tokenizer, prompt_text,
                                                timeout_sec=gen_timeout))
    finally:
        # Drop OUR refs before free_model's gc pass, or the 31B weights
        # survive into the next pass's load and two copies coexist.
        del model, tokenizer
        free_model()

    elapsed = time.time() - start
    return (responses, {"pass": pass_label, "elapsed_sec": elapsed,
                        "load_sec": load_sec, "count": len(responses)})


# ── Served-endpoint generation (never two loaders) ──────────────────────────
#
# 2026-09-03 04:31: the in-process path above loaded a SECOND copy of the
# 56 GB serving base beside the live :8741 mlx-server. Wired memory reached
# 94.8 GB, the server died with "[METAL] Insufficient Memory", became a ?E
# zombie holding its wired pages, and production was dark until a reboot.
#
# The served path generates THROUGH the production endpoint instead. The
# adapter delta still needs two arms, and the serving build's
# POST /v1/adapters/swap has no "unload", so the PRE arm swaps in a ZERO
# adapter: a twin of the serving adapter with every tensor zeroed. LoRA is
# additive (W·x + scale·(x·A)·B), so an all-zero A/B is the base model exactly
# — on any build that binds LoRA at all. Sequence: swap→zero, PRE, restore
# (verified via /v1/adapters/current), POST on the serving adapter.

DEFAULT_ZERO_ADAPTER_ROOT = Path.home() / ".human" / "eval" / "fidelity-zero-adapter"


def production_server_url(env: dict | None = None) -> str:
    """Base URL of the production mlx-server: HU_MLX_BASE_URL (any trailing /v1
    stripped), else http://127.0.0.1:<production_mlx_port>."""
    env = os.environ if env is None else env
    url = env.get("HU_MLX_BASE_URL", "").strip().rstrip("/")
    if url:
        return re.sub(r"/v1$", "", url)
    return f"http://127.0.0.1:{production_mlx_port(env)}"


def _http_json(method: str, url: str, body: dict | None = None, timeout: float = 30,
               headers: dict | None = None) -> tuple[int, dict | None]:
    """One JSON round-trip. Thin seam: tests patch THIS symbol. HTTP error
    statuses are returned, not raised; transport errors propagate."""
    data = json.dumps(body).encode("utf-8") if body is not None else None
    hdrs = {"Content-Type": "application/json", **(headers or {})}
    req = urllib.request.Request(url, data=data, method=method, headers=hdrs)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8")
            return resp.status, (json.loads(raw) if raw else None)
    except urllib.error.HTTPError as e:
        try:
            payload = json.loads(e.read().decode("utf-8"))
        except Exception:
            payload = None
        return e.code, payload


def served_endpoint_available(url: str, timeout: float = 5.0) -> bool:
    """True when a mlx-server with the adapter admin surface answers at url."""
    try:
        status, _ = _http_json("GET", f"{url}/v1/adapters/current", timeout=timeout)
    except (OSError, ValueError):
        return False
    return status == 200


def served_current_adapter(url: str) -> str | None:
    """GET /v1/adapters/current → adapter_path, or None when unanswerable."""
    try:
        status, body = _http_json("GET", f"{url}/v1/adapters/current", timeout=15)
    except (OSError, ValueError):
        return None
    if status != 200 or not isinstance(body, dict):
        return None
    return body.get("adapter_path")


def served_swap_adapter(url: str, adapter_path) -> tuple[bool, str]:
    """POST /v1/adapters/swap. Returns (ok, detail)."""
    try:
        status, body = _http_json("POST", f"{url}/v1/adapters/swap",
                                  body={"adapter_path": str(adapter_path)}, timeout=600)
    except (OSError, ValueError) as e:
        return False, f"swap transport error: {e}"
    if status != 200:
        return False, f"swap HTTP {status}: {str(body)[:200]}"
    return True, "ok"


def _same_path(a, b) -> bool:
    if not a or not b:
        return False
    try:
        return Path(a).expanduser().resolve() == Path(b).expanduser().resolve()
    except OSError:
        return False


def served_restore_adapter(url: str, expected, attempts: int = 3,
                           backoff_sec: float = 5.0) -> bool:
    """Swap `expected` back in and VERIFY /v1/adapters/current reports it.
    A swap that returns 200 but leaves a different adapter active counts as a
    failure — the artifact, not the report, decides."""
    for attempt in range(1, attempts + 1):
        ok, detail = served_swap_adapter(url, expected)
        if ok and _same_path(served_current_adapter(url), expected):
            return True
        print(f"[WARN] restore attempt {attempt}/{attempts} did not take: {detail}",
              flush=True)
        if attempt < attempts:
            time.sleep(backoff_sec * attempt)
    return False


def _read_safetensors_header(path: Path) -> tuple[dict, int]:
    with open(path, "rb") as f:
        n = struct.unpack("<Q", f.read(8))[0]
        return json.loads(f.read(n)), 8 + n


def ensure_zero_adapter(serving_dir, root=DEFAULT_ZERO_ADAPTER_ROOT) -> Path:
    """Build (or reuse) the zero-delta twin of `serving_dir` under `root`.

    Same tensor names, dtypes, shapes and offsets as the serving adapter's
    adapters.safetensors; every data byte zero (a sparse file — APFS stores
    the ~550 MB of zeros as nothing). adapter_config.json is copied so the
    directory passes the same shape checks the real adapter does. Reused
    when the cached twin's header matches; rewritten when it doesn't.
    """
    serving_dir = Path(serving_dir).expanduser()
    src_file = serving_dir / "adapters.safetensors"
    hdr, _ = _read_safetensors_header(src_file)
    shape_of = {k: (v.get("dtype"), v.get("shape"), v.get("data_offsets"))
                for k, v in hdr.items() if k != "__metadata__"}
    out_dir = Path(root).expanduser() / serving_dir.name
    out_file = out_dir / "adapters.safetensors"
    cfg_src, cfg_dst = serving_dir / "adapter_config.json", out_dir / "adapter_config.json"

    if out_file.exists():
        try:
            cached, _ = _read_safetensors_header(out_file)
            cached_shape = {k: (v.get("dtype"), v.get("shape"), v.get("data_offsets"))
                            for k, v in cached.items() if k != "__metadata__"}
        except (OSError, ValueError, struct.error):
            cached_shape = None
        if cached_shape == shape_of:
            if cfg_src.exists() and not cfg_dst.exists():
                shutil.copyfile(cfg_src, cfg_dst)
            return out_dir

    out_dir.mkdir(parents=True, exist_ok=True)
    data_len = max((v["data_offsets"][1] for v in hdr.values()
                    if isinstance(v, dict) and "data_offsets" in v), default=0)
    header = json.dumps(hdr).encode("utf-8")
    tmp = out_dir / f".adapters.safetensors.tmp-{os.getpid()}"
    with open(tmp, "wb") as f:
        f.write(struct.pack("<Q", len(header)))
        f.write(header)
        f.truncate(8 + len(header) + data_len)  # sparse zeros
    os.replace(tmp, out_file)
    if cfg_src.exists():
        shutil.copyfile(cfg_src, cfg_dst)
    return out_dir


def generate_served(url: str, prompt: str, max_tokens: int = 80,
                    timeout_sec: int = 600, retries: int = 3) -> str:
    """One generation through POST /v1/chat/completions on the served model.

    Same return contract as generate_inprocess(): model text or a
    SENTINEL_PREFIXES marker that is_sentinel() filters. Submitted at batch
    priority so a nightly never cuts ahead of a real conversation; a 503
    (admission queue full) is retried, anything else is a sentinel.
    """
    body = {"messages": [{"role": "user", "content": prompt}],
            "max_tokens": max_tokens, "temperature": 0.0, "stream": False}
    headers = {"X-HU-Priority": "batch"}
    last = "no attempt"
    for attempt in range(1, retries + 1):
        try:
            status, payload = _http_json("POST", f"{url}/v1/chat/completions",
                                         body=body, timeout=timeout_sec, headers=headers)
        except (TimeoutError, socket.timeout):
            return "[timeout]"
        except urllib.error.URLError as e:
            if isinstance(e.reason, (TimeoutError, socket.timeout)):
                return "[timeout]"
            return f"[gen_err: {str(e)[:100]}]"
        except Exception as e:  # noqa: BLE001 — per-prompt failures never raise
            return f"[gen_err: {str(e)[:100]}]"
        if status == 503:
            last = f"HTTP 503 {str(payload)[:60]}"
            time.sleep(5.0 * attempt)
            continue
        if status != 200:
            return f"[gen_err: HTTP {status} {str(payload)[:80]}]"
        try:
            text = payload["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError):
            return "[gen_err: malformed completion payload]"
        text = (text or "").strip()
        return text[-300:].strip() if text else "[empty]"
    return f"[gen_err: {last[:100]}]"


def run_served_pass(url: str, prompts: list[dict], pass_label: str,
                    gen_timeout: int = 600) -> tuple[list[str], dict]:
    """One arm of the comparison, generated through the served endpoint. The
    caller has already put the right adapter (zero twin, or serving) on the
    server; this function only generates."""
    start = time.time()
    responses = []
    for i, p in enumerate(prompts):
        prompt_text = p["prompt"] if isinstance(p, dict) else p
        print(f"  [{pass_label}] {i+1}/{len(prompts)} {prompt_text[:50]!r}...", flush=True)
        responses.append(generate_served(url, prompt_text, timeout_sec=gen_timeout))
    return (responses, {"pass": pass_label, "elapsed_sec": time.time() - start,
                        "count": len(responses), "server_url": url})



def main():
    ap = argparse.ArgumentParser(
        description="Nightly fidelity eval harness + SOTA gate",
    )
    ap.add_argument(
        "--adapter-path",
        type=Path,
        default=None,
        help="Explicit LoRA adapter override. Default: resolve the SERVING "
             "adapter dynamically (live mlx-server process, then config.json "
             "personalization.lora_adapter_path).",
    )
    ap.add_argument(
        "--resolve-only",
        action="store_true",
        help="Print the resolved serving adapter path and exit (0 resolved, 3 not)",
    )
    ap.add_argument(
        "--dump-responses",
        action="store_true",
        help="Include raw pre/post responses in the verdict JSON (debugging: "
             "proves the two passes actually produced different outputs)",
    )
    ap.add_argument(
        "--min-prompts",
        type=int,
        default=20,
        help="Minimum held-out prompts required (default 20; lower only for "
             "manual small-n smoke runs — results below 20 are not gate-grade)",
    )
    ap.add_argument(
        "--model-id",
        default=None,
        help="HuggingFace base model ID. Default: resolve from the live "
             f"mlx-server --model (else {DEFAULT_MODEL}) so the adapter is "
             "measured against the base it actually serves on.",
    )
    ap.add_argument(
        "--speaker-model",
        type=Path,
        default=DEFAULT_SPEAKER_MODEL_PATH,
        help="Speaker-id P(Seth) classifier JSON blended with the shape score "
             f"(default: {DEFAULT_SPEAKER_MODEL_PATH}). The shape classifier "
             "alone saturates at 1.0 on clean casual text (2026-07-16: both "
             "passes scored mean 1.0 across 29 real generations), so without "
             "this component the delta gate cannot PASS. Missing model → "
             "loud shape-only fallback.",
    )
    ap.add_argument(
        "--gen-timeout",
        type=int,
        default=600,
        help="Per-prompt wall-clock guard in seconds (default 600). In-process "
             "generations run on a warm model and finish in seconds — this only "
             "fires on a hang. With --subprocess-gen it is the per-subprocess "
             "kill deadline, which must also cover a cold model load.",
    )
    ap.add_argument(
        "--no-registry",
        action="store_true",
        help="Run the full eval but do NOT record the verdict in the adapter "
             "registry. Use for smoke/manual runs (e.g. --min-prompts 5): "
             "small-n verdicts recorded as real registry entries are "
             "indistinguishable from gate-grade nightlies in promotion history.",
    )
    ap.add_argument(
        "--subprocess-gen",
        action="store_true",
        help="Legacy generation path: spawn `python3 -m mlx_lm generate` per "
             "prompt (58 cold loads of the 31B model per night, ~3h wall). "
             "Fallback only — the default loads the model once per pass "
             "in-process via the mlx_lm Python API.",
    )
    ap.add_argument(
        "--gen",
        choices=("auto", "served", "inprocess"),
        default=os.environ.get("HU_FIDELITY_GEN", "auto"),
        help="Where generation runs. served: through the production mlx-server "
             "(HU_MLX_BASE_URL, else :8741) with the PRE arm on a zero-delta "
             "adapter swapped in via /v1/adapters/swap. inprocess: mlx_lm.load "
             "in this process — REFUSED while a server is up (never two "
             "loaders; 2026-09-03 incident). auto (default, env "
             "HU_FIDELITY_GEN): served when a server answers, else inprocess.",
    )
    ap.add_argument(
        "--zero-adapter-root",
        type=Path,
        default=DEFAULT_ZERO_ADAPTER_ROOT,
        help="Where the zero-delta twin of the serving adapter is cached for "
             f"the served PRE arm (default: {DEFAULT_ZERO_ADAPTER_ROOT})",
    )
    ap.add_argument(
        "--held-out-fixture",
        type=Path,
        default=DEFAULT_FIXTURE,
        help=f"Path to heldout-prompts.jsonl (default: {DEFAULT_FIXTURE})",
    )
    ap.add_argument(
        "--held-out-db-path",
        type=Path,
        help="Alternative: query prompts from this DB (not implemented; fixture takes precedence)",
    )
    ap.add_argument(
        "--output-json",
        type=Path,
        help="Write gate verdict JSON to this path (default: stdout only)",
    )
    ap.add_argument(
        "--log-dir",
        type=Path,
        default=DEFAULT_LOG_DIR,
        help=f"Directory for logs (default: {DEFAULT_LOG_DIR})",
    )
    args = ap.parse_args()

    # Ensure log dir exists before anything that might emit a verdict JSON
    args.log_dir.mkdir(parents=True, exist_ok=True)

    # Resolve the adapter under test: explicit override wins, else the one
    # actually serving. A hardcoded adapter name went stale in 2026-07 and
    # skipped silently for 13 nights — dynamic resolution is the default.
    if args.adapter_path is None:
        resolved, source = resolve_serving_adapter()
        if args.resolve_only:
            # stdout is a path-or-empty contract for shell callers; noise → stderr
            if resolved is None:
                print(f"[SKIP] FIDELITY_SKIP no serving adapter resolvable ({source})",
                      file=sys.stderr)
                return EXIT_SKIP
            print(resolved)
            return EXIT_PASS
        if resolved is None:
            return emit_skip(f"no serving adapter resolvable ({source})", args.output_json)
        print(f"[INFO] Resolved serving adapter via {source}: {resolved}", flush=True)
        args.adapter_path = resolved
    elif args.resolve_only:
        print(args.adapter_path)
        return EXIT_PASS

    if args.model_id is None:
        args.model_id = resolve_serving_model()
        print(f"[INFO] Resolved serving base model: {args.model_id}", flush=True)

    # A cross-family pair cannot produce a delta (the LoRA never binds), so any
    # verdict from it would be fiction. DEFER — do not record.
    if base_adapter_family_mismatch(args.model_id, args.adapter_path):
        reason = (f"base {args.model_id} and adapter {args.adapter_path} are from "
                  f"different model families — the adapter cannot bind, so any "
                  f"delta would be an artifact")
        print(f"[DEFERRED] FIDELITY_DEFERRED {reason}", file=sys.stderr, flush=True)
        verdict = {
            "timestamp": datetime.now().isoformat(),
            "verdict": "DEFERRED",
            "reason": reason,
            "exit_code": EXIT_DEFERRED,
            "model_id": args.model_id,
            "adapter_path": str(args.adapter_path),
        }
        if args.output_json:
            args.output_json.write_text(json.dumps(verdict, indent=2))
        return EXIT_DEFERRED

    # Load held-out prompts
    print(f"[INFO] Loading held-out prompts from {args.held_out_fixture}", flush=True)
    prompts = load_held_out_prompts_from_jsonl(str(args.held_out_fixture))

    if not prompts:
        return emit_skip(
            f"Held-out prompts unavailable or empty: {args.held_out_fixture}",
            args.output_json,
        )

    print(f"[INFO] Loaded {len(prompts)} held-out prompts (min {args.min_prompts} required)", flush=True)
    if len(prompts) < args.min_prompts:
        return emit_skip(
            f"Insufficient held-out prompts: {len(prompts)} < {args.min_prompts}",
            args.output_json,
        )

    # Verify adapter exists (explicit --adapter-path may point anywhere)
    if not args.adapter_path.exists():
        return emit_skip(f"Adapter not found: {args.adapter_path}", args.output_json)

    # ── Where does generation run? ──────────────────────────────────────────
    # A server on the production port means the 56 GB base is ALREADY wired in
    # memory; loading it again here is the 2026-09-03 outage. HU_MLX_BASE_URL
    # pins the served path outright; otherwise auto probes the port.
    server_url = production_server_url()
    base_url_pinned = bool(os.environ.get("HU_MLX_BASE_URL", "").strip())
    server_up = served_endpoint_available(server_url)
    mode = args.gen
    if mode == "auto":
        mode = "served" if (server_up or base_url_pinned) else "inprocess"
    if mode == "inprocess" and args.subprocess_gen:
        mode = "subprocess"
    if mode != "served" and (server_up or base_url_pinned):
        return emit_deferred(
            f"refusing to start a second model loader ({mode}) beside the live "
            f"mlx-server at {server_url} — never two loaders (2026-09-03: the "
            f"in-process load took production down); run with --gen served, or "
            f"stop the server first",
            args.output_json, generation={"mode": mode, "server_url": server_url},
        )
    if mode == "served" and not server_up:
        return emit_deferred(
            f"served generation requested but no mlx-server answers "
            f"/v1/adapters/current at {server_url}",
            args.output_json, generation={"mode": mode, "server_url": server_url},
        )
    gen_info = {"mode": mode, "server_url": server_url if mode == "served" else None,
                "pre_arm_adapter": None, "adapter_restored": None}
    print(f"[INFO] generation mode: {mode}"
          + (f" via {server_url}" if mode == "served" else ""), flush=True)

    if mode == "served":
        # The server must be serving the adapter under test: "restore" below
        # puts args.adapter_path back, so if something else is active we would
        # be measuring one adapter and installing another. DEFER before any swap.
        current = served_current_adapter(server_url)
        if not _same_path(current, args.adapter_path):
            return emit_deferred(
                f"served mlx-server at {server_url} reports adapter "
                f"{current!r}, not the adapter under test {args.adapter_path} — "
                f"not swapping",
                args.output_json, generation=gen_info,
            )
        try:
            zero_dir = ensure_zero_adapter(args.adapter_path, args.zero_adapter_root)
        except (OSError, ValueError, KeyError, struct.error) as e:
            return emit_deferred(f"could not build zero adapter for PRE arm: {e}",
                                 args.output_json, generation=gen_info)
        gen_info["pre_arm_adapter"] = str(zero_dir)

        print(f"\n=== PRE PASS (served; zero-delta adapter = base) ===", flush=True)
        ok, detail = served_swap_adapter(server_url, zero_dir)
        if not ok or not _same_path(served_current_adapter(server_url), zero_dir):
            # The server reverts itself on a failed swap; confirm, then defer.
            gen_info["adapter_restored"] = _same_path(
                served_current_adapter(server_url), args.adapter_path)
            if not gen_info["adapter_restored"]:
                print("[ERROR] FIDELITY_RESTORE_FAILED serving adapter is not active "
                      "after a failed PRE swap", flush=True)
            return emit_deferred(f"PRE swap to zero adapter did not take: {detail}",
                                 args.output_json, generation=gen_info)
        pre_error = None
        try:
            pre_responses, pre_stats = run_served_pass(
                server_url, prompts, "PRE (base)", gen_timeout=args.gen_timeout)
        except Exception as e:  # noqa: BLE001 — restore first, report second
            pre_error = e
        finally:
            gen_info["adapter_restored"] = served_restore_adapter(
                server_url, args.adapter_path)
        if not gen_info["adapter_restored"]:
            msg = (f"FIDELITY_RESTORE_FAILED serving adapter {args.adapter_path} could "
                   f"not be restored on {server_url} after the PRE arm — production "
                   f"may be running on the zero adapter {zero_dir}; restore by hand: "
                   f"POST {server_url}/v1/adapters/swap "
                   f'{{"adapter_path": "{args.adapter_path}"}}')
            print(f"[ERROR] {msg}", flush=True)
            print(f"[ERROR] {msg}", file=sys.stderr, flush=True)
            return emit_deferred(msg, args.output_json, generation=gen_info)
        if pre_error is not None:
            return emit_deferred(f"PRE pass failed: {str(pre_error)[:200]}",
                                 args.output_json, generation=gen_info)
        print(f"[INFO] serving adapter restored and verified: {args.adapter_path}",
              flush=True)

        print(f"\n=== POST PASS (served; serving adapter) ===", flush=True)
        try:
            post_responses, post_stats = run_served_pass(
                server_url, prompts, "POST (adapter)", gen_timeout=args.gen_timeout)
        except Exception as e:  # noqa: BLE001
            return emit_deferred(f"POST pass failed: {str(e)[:200]}",
                                 args.output_json, generation=gen_info)
    else:
        # PRE pass (base model only).
        # adapter_path=None is passed EXPLICITLY rather than left to the default:
        # this is the contract that makes the run a comparison at all, and it should
        # be legible at the call site. Nothing downstream can re-introduce an
        # adapter — load_model() forwards this straight to mlx_lm.load() and never
        # consults config.json, so the config-inheritance path that bit mlx-server.py
        # (a gemma adapter silently applied to GLM) has no equivalent here.
        print(f"\n=== PRE PASS (base model) ===", flush=True)
        try:
            pre_responses, pre_stats = run_eval_pass(
                args.model_id, prompts, adapter_path=None, gen_timeout=args.gen_timeout,
                use_subprocess=args.subprocess_gen,
            )
        except Exception as e:
            return emit_deferred(f"PRE pass failed: {str(e)[:200]}",
                                 args.output_json, generation=gen_info)

        # POST pass (base + adapter)
        print(f"\n=== POST PASS (base + adapter) ===", flush=True)
        try:
            post_responses, post_stats = run_eval_pass(
                args.model_id, prompts, adapter_path=str(args.adapter_path),
                gen_timeout=args.gen_timeout,
                use_subprocess=args.subprocess_gen,
            )
        except Exception as e:
            return emit_deferred(f"POST pass failed: {str(e)[:200]}",
                                 args.output_json, generation=gen_info)

    # Drop pairs where either response is an error sentinel — sentinels like
    # '[timeout]' score 1.0 on the shape classifier and poisoned 10 nights of
    # verdicts in 2026-07. If too few real pairs survive, DEFER: this is a
    # broken harness, not a measured adapter.
    n_sentinel_pre = sum(1 for r in pre_responses if is_sentinel(r))
    n_sentinel_post = sum(1 for r in post_responses if is_sentinel(r))
    valid_idx = [i for i in range(len(prompts))
                 if not is_sentinel(pre_responses[i]) and not is_sentinel(post_responses[i])]
    if n_sentinel_pre or n_sentinel_post:
        print(f"[WARN] sentinel responses excluded: pre={n_sentinel_pre}, "
              f"post={n_sentinel_post}, valid_pairs={len(valid_idx)}/{len(prompts)}",
              flush=True)

    min_valid = max(1, (len(prompts) * 4 + 4) // 5)  # ceil(80%)
    if len(valid_idx) < min_valid:
        reason = (f"only {len(valid_idx)}/{len(prompts)} valid pairs "
                  f"(pre sentinels={n_sentinel_pre}, post sentinels={n_sentinel_post}); "
                  f"generation is failing, not the adapter")
        verdict = {
            "timestamp": datetime.now().isoformat(),
            "verdict": "DEFERRED",
            "reason": reason,
            "exit_code": EXIT_DEFERRED,
        }
        print(f"[DEFERRED] FIDELITY_DEFERRED {reason}", flush=True)
        if args.output_json:
            args.output_json.write_text(json.dumps(verdict, indent=2))
        return EXIT_DEFERRED

    pre_valid = [pre_responses[i] for i in valid_idx]
    post_valid = [post_responses[i] for i in valid_idx]

    # ── The adapter must actually have changed something ────────────────────
    # Byte-identical passes mean the LoRA applied no delta at all. mlx_lm loads
    # adapter weights with strict=False and zero-initialises lora_b, so a
    # non-binding adapter is a silent no-op (see responses_identical). Every
    # degenerate nightly in the record has this signature:
    #   2026-07-12..07-25  pre == post == 1.0     (all sentinels, 14 nights)
    #   2026-07-27 03:14   pre == post == 0.3178  (GLM adapter on a gemma base)
    # All 15 were recorded as SKIP — "the adapter did not help" — when the truth
    # was that no measurement occurred. DEFER instead, and say which.
    declared_base = adapter_declared_base(args.adapter_path)
    if declared_base and declared_base != args.model_id:
        print(f"[WARN] adapter declares base {declared_base!r} but this run uses "
              f"{args.model_id!r}; a different quantization of one base can still "
              f"bind, so this is advisory — the no-op check below is the gate.",
              flush=True)
    if responses_identical(pre_valid, post_valid):
        return emit_deferred(
            f"PRE and POST produced byte-identical output on all "
            f"{len(pre_valid)} valid prompts — the adapter applied no delta, so "
            f"no comparison was measured. Most likely the adapter did not bind "
            f"to this base (mlx_lm loads adapter weights with strict=False and "
            f"fails silently). base={args.model_id} "
            f"adapter={args.adapter_path} adapter_declared_base={declared_base}"
            + (f". SERVED MODE: the mlx-server at {server_url} applies adapters "
               f"with model.load_weights(strict=False) and never injects LoRA "
               f"layers, so lora_* tensors are dropped silently and BOTH arms "
               f"are the base model — the serving adapter is not active"
               if mode == "served" else ""),
            args.output_json,
            generation=gen_info,
            model_id=args.model_id,
            adapter_path=str(args.adapter_path),
            adapter_declared_base=declared_base,
            n_valid_pairs=len(pre_valid),
        )
    n_differing = sum(1 for a, b in zip(pre_valid, post_valid) if a != b)

    # Score responses. Blend shape (AI-tell penalties) with speaker-id P(Seth):
    # shape alone saturates at 1.0 on clean casual text, which made the delta
    # gate unwinnable on 2026-07-16 (both passes mean 1.0 across 29 prompts).
    speaker_model = load_speaker_model(args.speaker_model)
    if speaker_model is None:
        # /tmp model is wiped on reboot — degrade loudly, never silently:
        # shape-only deltas saturate to ~0, so this run can at best SKIP.
        print(f"[WARN] FIDELITY_SCORER_DEGRADED speaker-id model unavailable at "
              f"{args.speaker_model}; falling back to shape-only scoring, which "
              f"saturates at 1.0 on clean casual text — the delta gate cannot "
              f"PASS. Retrain via: python3 scripts/personaeval_speaker_id.py "
              f"--train --out {args.speaker_model}", flush=True)
    scorer_mode = "blended" if speaker_model is not None else "shape-only"

    print(f"\n=== SCORING ({len(valid_idx)} valid pairs, scorer={scorer_mode}) ===", flush=True)
    pre_classifications, pre_mean = compute_persona_fidelity_scores(
        pre_valid, channel="imessage", speaker_model=speaker_model)
    post_classifications, post_mean = compute_persona_fidelity_scores(
        post_valid, channel="imessage", speaker_model=speaker_model)

    print(f"PRE mean score:  {pre_mean:.3f}", flush=True)
    print(f"POST mean score: {post_mean:.3f}", flush=True)

    # Check for zero scores
    if pre_mean == 0.0 or post_mean == 0.0:
        verdict = {
            "timestamp": datetime.now().isoformat(),
            "verdict": "FAIL",
            "reason": f"Zero mean score: pre={pre_mean}, post={post_mean}. No valid responses.",
            "exit_code": 1,
        }
        print(f"[FAIL] {verdict['reason']}")
        if args.output_json:
            args.output_json.write_text(json.dumps(verdict, indent=2))
        return 1

    # Compute per-prompt deltas for bootstrap CI
    deltas = [post_classifications[i]["score"] - pre_classifications[i]["score"]
              for i in range(len(pre_classifications))]
    delta_mean = statistics.mean(deltas)

    # Bootstrap CI on deltas
    print(f"\n=== BOOTSTRAP CI (N=100 resamples) ===", flush=True)
    delta_mean_boot, delta_lo, delta_hi = bootstrap_ci(
        deltas, n_resamples=100, confidence=CONFIDENCE
    )
    stderr = (delta_hi - delta_lo) / (2 * 1.96)  # approximate stderr from CI width

    print(f"Delta mean:  {delta_mean:.3f}", flush=True)
    print(f"Delta CI:    [{delta_lo:.3f}, {delta_hi:.3f}]", flush=True)
    print(f"Stderr est:  {stderr:.4f}", flush=True)

    # SOTA gate logic
    # (AC-9.5) Statistical: post_mean > pre_mean + 1.96 * stderr
    # (AC-9.6) Practical: delta >= 0.05
    # PASS iff BOTH hold; else FAIL or SKIP
    print(f"\n=== SOTA GATE ===", flush=True)

    stat_threshold = pre_mean + 1.96 * stderr
    stat_pass = post_mean > stat_threshold
    prac_pass = delta_mean >= PRACTICAL_DELTA_FLOOR

    print(f"Statistical (α={ALPHA_ONESIDED}): post_mean ({post_mean:.3f}) > "
          f"pre_mean ({pre_mean:.3f}) + 1.96*stderr ({1.96*stderr:.4f}) = {stat_threshold:.3f}")
    print(f"  → {['FAIL', 'PASS'][stat_pass]}", flush=True)

    print(f"Practical (floor={PRACTICAL_DELTA_FLOOR}): delta_mean ({delta_mean:.3f}) >= {PRACTICAL_DELTA_FLOOR}")
    print(f"  → {['FAIL', 'PASS'][prac_pass]}", flush=True)

    if stat_pass and prac_pass:
        final_verdict = "PASS"
        exit_code = EXIT_PASS
    elif not prac_pass:
        # No measurable improvement over base — a real measurement, but not a
        # promotable one. Exit 3 (not 0) so a SKIP streak is visible to callers.
        final_verdict = "SKIP"
        exit_code = EXIT_SKIP
    else:
        final_verdict = "FAIL"
        exit_code = EXIT_FAIL

    # Detailed verdict JSON
    verdict = {
        "timestamp": datetime.now().isoformat(),
        "verdict": final_verdict,
        "exit_code": exit_code,
        "n_prompts": len(prompts),
        "n_valid_pairs": len(valid_idx),
        "n_sentinel": {"pre": n_sentinel_pre, "post": n_sentinel_post},
        "scorer": {
            "mode": scorer_mode,
            "shape_weight": SHAPE_WEIGHT if scorer_mode == "blended" else 1.0,
            "speaker_weight": SPEAKER_WEIGHT if scorer_mode == "blended" else 0.0,
            "speaker_model_path": str(args.speaker_model) if scorer_mode == "blended" else None,
        },
        "gen_timeout_sec": args.gen_timeout,
        "generation": gen_info,
        "model_id": args.model_id,
        "adapter_path": str(args.adapter_path),
        # Positive evidence that the two passes were actually different runs.
        # Without it, a reader of a SKIP cannot distinguish "the adapter did not
        # help" from "the adapter was never applied" — the two are identical on
        # every other field. n_differing_pairs == 0 is unreachable here (the
        # guard above DEFERs first); it is recorded so the claim is checkable
        # rather than implied.
        "differentiation": {
            "n_differing_pairs": n_differing,
            "n_valid_pairs": len(valid_idx),
            "adapter_declared_base": declared_base,
        },
        "pre": {
            "mean_score": round(pre_mean, 4),
            "elapsed_sec": pre_stats["elapsed_sec"],
        },
        "post": {
            "mean_score": round(post_mean, 4),
            "elapsed_sec": post_stats["elapsed_sec"],
        },
        "delta": {
            "mean": round(delta_mean, 4),
            "ci_lower": round(delta_lo, 4),
            "ci_upper": round(delta_hi, 4),
            "stderr_est": round(stderr, 4),
        },
        "gate": {
            "statistical_pass": stat_pass,
            "statistical_threshold": round(stat_threshold, 4),
            "statistical_alpha": ALPHA_ONESIDED,
            "practical_pass": prac_pass,
            "practical_floor": PRACTICAL_DELTA_FLOOR,
        },
    }

    if args.dump_responses:
        verdict["responses"] = {"pre": pre_valid, "post": post_valid}

    print(f"\n=== FINAL VERDICT: {final_verdict} ===", flush=True)
    if final_verdict == "SKIP":
        print(f"[SKIP] FIDELITY_SKIP no measurable improvement "
              f"(delta_mean={delta_mean:.4f} < floor={PRACTICAL_DELTA_FLOOR})", flush=True)

    # Output verdict JSON
    if args.output_json:
        args.output_json.write_text(json.dumps(verdict, indent=2))
        print(f"[INFO] Verdict written to {args.output_json}", flush=True)

    # Also log to structured log file
    log_file = args.log_dir / f"eval-fidelity-{datetime.now().strftime('%Y-%m-%d')}.json"
    log_file.write_text(json.dumps(verdict, indent=2))
    print(f"[INFO] Log written to {log_file}", flush=True)

    # Record evaluation result to adapter registry. A SKIP records score=None:
    # 13 nightly SKIPs once landed as {"score": 1.0, "verdict": "SKIP"} and
    # read as perfect evals in the registry history. Full scores stay in the
    # verdict JSON above; the registry score is only meaningful for PASS/FAIL.
    if args.no_registry:
        print("[INFO] --no-registry: verdict NOT recorded in adapter registry",
              flush=True)
        return exit_code
    try:
        adapter_name = Path(args.adapter_path).name
        adapter_registry.record_eval(
            adapter_id=adapter_name,
            eval_name="fidelity-nightly",
            score=post_mean if final_verdict != "SKIP" else None,
            verdict=final_verdict,
            timestamp=datetime.now().isoformat()
        )
        print(f"[INFO] Eval result recorded to adapter registry", flush=True)
    except Exception as e:
        print(f"[WARN] Failed to record eval to registry: {e}", file=sys.stderr, flush=True)

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
