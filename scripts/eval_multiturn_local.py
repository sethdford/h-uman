#!/usr/bin/env python3
"""Sustained multi-turn coherence eval (on-device).

Drives the LOCAL mlx-server through deep (20–30 turn) conversations and
scores retention (judge + anchors), voice drift (judge, over distance), and
latency (wall-clock per turn: pathology ceiling + median growth). Emits a
verdict JSON. Nightly/manual tool — not a per-PR CI gate.

LATENCY — empirical reality (2026-05-29): the live realtime server buffers,
i.e. it GENERATES the full reply then emits it as one SSE chunk, so measured
time-to-first-token equals total turn latency (TTFT == total). The harness
records both (series_ms gated, total_series_ms diagnostic) so it auto-separates
if a future server streams real tokens, but today there is no early-token
prefill signal. Per-turn latency is generation-bound and dominated by reply
length, not conversation depth (measured envelope min 5.4 s / p50 16.6 s /
max 78.2 s, KV-cache on). The latency axis is therefore a coarse regression
guard, not a fine "no cliff" proof: ceiling = a hang/runaway bound (90 s),
growth = median-of-thirds ≤ 1.0 (catches catastrophic blowup only). See the
LATENCY_CEILING_MS comment block for the full rationale.

Usage:
  python3 scripts/eval_multiturn_local.py \\
    --server-url http://127.0.0.1:8741 \\
    --output-json ~/.human/logs/eval-multiturn-local.json

Exit codes:
  0 = run PASS
  1 = run FAIL (an axis failed, or a scenario fell below the hard retention floor)
  2 = DEFERRED (mlx-server unreachable)
  3 = SKIPPED (judge/ADC unavailable; latency axis ran, qualitative axes skipped)
"""
import argparse
import json
import statistics
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from eval_multiturn import (  # noqa: E402  (after sys.path.insert)
    call_gemini, _get_adc_token, evaluate_conversation, EVAL_DIMENSIONS,
)
import multiturn_scenarios_deep

# --- Thresholds (LOCKED 2026-05-28 against the first live calibration run) ---
# Calibration run: 6 deep scenarios (28–30 turns each) vs the local
# gemma-4-31b-it-4bit mlx-server. Verdict artifact:
#   docs/superpowers/specs/results/2026-05-28-multiturn-local-verdict.json
#
# RETENTION (0.85 / 0.70): well-calibrated and discriminating. The model held
#   context across 28–30 turns — 5/6 scenarios scored 1.00; only banter_humor
#   missed a single anchor (0.67). Thresholds KEPT: they pass the strong
#   scenarios and flag the genuine miss without being arbitrary.
RETENTION_RATE_MIN     = 0.85
RETENTION_HARD_FLOOR   = 0.70
# VOICE (drift 0.10 + no-hard-AI): KEPT, but the first run exposed a HARNESS
#   fidelity gap, not a model failure — run_scenario sends only user turns with
#   NO persona system prompt, so the raw mlx-server replies as a generic
#   assistant and the judge (correctly) flips every late window to a hard "AI"
#   verdict. The drift comparison itself mostly passed (e.g. casual 1→3,
#   debate 3→3). Fixing this requires injecting the production persona system
#   prompt each turn (tracked as a follow-up); the threshold is sound once the
#   harness reproduces production prompt assembly.
VOICE_DRIFT_TOL        = 0.10
# LATENCY — EMPIRICAL FINDING (2026-05-29): the live realtime server does NOT
#   stream token-by-token. With stream:true it honors the SSE protocol but
#   GENERATES THE FULL REPLY, then emits it as one final chunk. So the measured
#   time-to-first-token EQUALS total turn latency on this server (TTFT == total).
#   The harness still records both (series_ms = gated, total_series_ms =
#   diagnostic) so it auto-separates IF a future server streams real tokens —
#   but today there is no early-token signal to isolate the prefill cliff from.
#
#   Consequence for calibration (measured on the 2026-05-28 full 6-scenario run,
#   173 turns): per-turn latency is generation-bound and DOMINATED BY REPLY
#   LENGTH, not conversation depth. Observed envelope: min 5.4 s, p50 16.6 s,
#   p90 34.3 s, p95 52.2 s, p99 74.6 s, max 78.2 s, with KV-cache ON
#   (--kv-bits 4). There is no monotonic depth cliff to detect: late-turn spikes
#   are long replies landing late by chance, not re-prefill cost.
#
#   So the latency axis is recalibrated to two HONEST, defensible signals:
#     1. CEILING = a PATHOLOGY/HANG bound, not a prefill bound. 90 s sits above
#        the 78.2 s longest legitimate reply, so it only trips on a genuine
#        runaway / non-terminating stream (the failure mode the rung-1/2
#        runaway guards exist to prevent). A regression there re-trips this gate.
#     2. GROWTH = median-of-thirds (NOT mean — see latency_growth), tolerance
#        1.0 (late median ≤ 2x early median). On the real data median-thirds
#        growth ranged +0.07..+0.84 across all 6 scenarios with NO architectural
#        cliff, so 1.0 passes all legitimate runs while still catching a
#        catastrophic monotonic re-prefill blowup (2x-5x). Growth is a coarse
#        regression guard here, not a fine "no cliff" proof — that proof needs a
#        server that streams real first tokens (then TTFT separates from total).
LATENCY_CEILING_MS     = 90000.0  # pathology/hang bound (>78.2 s max legit reply)
LATENCY_MAX_GROWTH     = 1.00     # median-of-thirds; catches catastrophic blowup
RUN_PASS_MIN_SCENARIOS = 5


def _thirds(series):
    """Split a list into (first_third, last_third) by index."""
    n = len(series)
    k = max(1, n // 3)
    return series[:k], series[-k:]


def latency_ceiling_violations(series_ms, ceiling_ms):
    """Return the list of turn indices whose latency exceeds the ceiling."""
    return [i for i, v in enumerate(series_ms) if v > ceiling_ms]


def latency_growth(series_ms):
    """Fractional growth of last-third vs first-third latency, by MEDIAN.

    Uses the MEDIAN of each third, not the mean. On a generation-bound server
    (see the LATENCY comment block) per-turn latency is dominated by reply
    length, so a single 78 s outlier landing in the last third would inflate a
    mean-based slope to a false "cliff". The median of each third is robust to
    those length spikes and only moves when the WHOLE late distribution shifts
    up — which is what an actual missing-prefix-cache cliff looks like.

    Returns 0.0 for an empty or single-element series. A return of 1.0 means
    the late turns' median is 2x the early turns' median.
    """
    if len(series_ms) < 2:
        return 0.0
    first, last = _thirds(series_ms)
    fmed = statistics.median(first)
    if fmed == 0:
        return 0.0
    return (statistics.median(last) - fmed) / fmed


def latency_ok(series_ms, ceiling_ms, max_growth):
    """Gate latency on absolute ceiling AND bounded growth.

    Returns (ok: bool, detail: dict).
    """
    violations = latency_ceiling_violations(series_ms, ceiling_ms)
    growth = latency_growth(series_ms)
    ok = (not violations) and (growth <= max_growth)
    detail = {
        "ceiling_ms": ceiling_ms,
        "ceiling_violations": violations,
        "growth": growth,
        "max_growth": max_growth,
        "series_ms": series_ms,
    }
    return ok, detail


def retention_rate(anchor_results):
    """Fraction of anchors the judge marked retained. Empty → 0.0."""
    if not anchor_results:
        return 0.0
    return sum(1 for r in anchor_results if r) / len(anchor_results)


def count_empty_replies(responses_by_turn):
    """Count assistant turns that returned no visible content.

    An empty reply (content stripped to "") means the model produced a turn
    with zero user-visible text. On the v4-repair adapter this is the
    thinking-starvation signature: at certain conversation depths the whole
    generation budget is consumed by (subsequently stripped) reasoning tokens,
    leaving nothing to send. Empirically (2026-05-28 partial live run) these
    cluster at a depth window (turns ~11–15) and recover afterward, and the
    empty turns are the SLOWEST (44–52 s vs 31.8 s p50) — long generation,
    zero output — confirming the budget went to thinking.

    This is a DIAGNOSTIC, not a gate. The impact is already penalized by the
    retention and voice gates (an empty probe-turn fails its anchor; an empty
    late-third turn tanks the voice score). Surfacing count/turns/rate explains
    WHY those gates moved, and gives the nightly run a baseline from which a
    dedicated empty-rate gate could later be calibrated.

    Returns a dict: {"count": int, "turns": [1-indexed ints], "rate": float}.
    """
    empties = sorted(t for t, c in responses_by_turn.items() if not (c or "").strip())
    total = len(responses_by_turn)
    return {
        "count": len(empties),
        "turns": empties,
        "rate": (len(empties) / total) if total else 0.0,
    }


def voice_normalize(overall_score_1_to_10):
    """Normalize a judge overall_score (1–10) to [0,1]."""
    return float(overall_score_1_to_10) / 10.0


def voice_drift_ok(first_third_norm, last_third_norm, tol, any_hard_ai):
    """Voice axis passes when late-conversation voice has not decayed.

    Fails if the last-third normalized score dropped more than `tol` below the
    first-third, OR any late turn flipped to a hard AI verdict.
    """
    if any_hard_ai:
        return False
    return last_third_norm >= (first_third_norm - tol)


def scenario_verdict(name, retention, voice_pass, voice_detail, latency_pass,
                     latency_detail, empty_replies=None):
    """Assemble a single scenario's per-axis verdict. All three axes must pass.

    `empty_replies` is the diagnostic dict from count_empty_replies (count/turns/
    rate). It is recorded but does NOT affect `passed` — see count_empty_replies
    for why empties are a diagnostic, not a gate.
    """
    retention_pass = retention >= RETENTION_RATE_MIN
    passed = retention_pass and voice_pass and latency_pass
    if empty_replies is None:
        empty_replies = {"count": 0, "turns": [], "rate": 0.0}
    return {
        "scenario": name,
        "retention": {"rate": retention, "min": RETENTION_RATE_MIN, "passed": retention_pass},
        "voice": {"passed": voice_pass, **voice_detail},
        "latency": {"passed": latency_pass, **latency_detail},
        "empty_replies": empty_replies,
        "passed": passed,
    }


def run_verdict(scenario_verdicts):
    """Aggregate scenario verdicts into the run-level verdict.

    Run passes when ≥ RUN_PASS_MIN_SCENARIOS scenarios pass AND no scenario
    fell below RETENTION_HARD_FLOOR (a catastrophic-retention veto).
    """
    passed_count = sum(1 for sv in scenario_verdicts if sv["passed"])
    hard_floor_veto = any(
        sv["retention"]["rate"] < RETENTION_HARD_FLOOR for sv in scenario_verdicts)
    run_passed = (passed_count >= RUN_PASS_MIN_SCENARIOS) and not hard_floor_veto
    return {
        "scenarios": scenario_verdicts,
        "scenarios_passed": passed_count,
        "scenarios_total": len(scenario_verdicts),
        "min_to_pass": RUN_PASS_MIN_SCENARIOS,
        "hard_floor_veto": hard_floor_veto,
        "run_passed": run_passed,
    }


def write_verdict(verdict, path):
    """Write the verdict JSON, stamping generated_at. Creates parent dirs."""
    out = dict(verdict)
    out.setdefault("generated_at", datetime.now(timezone.utc).isoformat())
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(out, indent=2))


class BackendUnreachable(RuntimeError):
    """Raised when the local mlx-server cannot be reached. Never fall back to cloud."""


class JudgeUnavailable(RuntimeError):
    """Raised when the cloud judge fails mid-run (ADC revoked, malformed reply,
    empty result). Distinct from BackendUnreachable: the model-under-test is
    fine, but the qualitative axes cannot be scored. Caught once in main() so a
    judge failure degrades to SKIPPED rather than crashing or silently scoring 0."""


class LocalBackend:
    """Talks to the local mlx-server's OpenAI-compatible endpoint.

    Sends the FULL accumulated history each turn (mirrors compatible.c — no
    server-side caching), which is what makes the latency growth signal real.
    Streams the reply (SSE) so the caller can time TIME-TO-FIRST-TOKEN — the
    prefill-bound latency that grows with history (the real "cliff").
    """
    def __init__(self, url, model="default", temperature=0.9, timeout=120):
        self.url = url.rstrip("/")
        self.model = model
        self.temperature = temperature
        self.timeout = timeout

    def chat(self, messages):
        """Stream a reply, return (content, first_token_ms, total_ms).

        first_token_ms is the wall-clock time-to-first-content-token (TTFT) —
        the prefill cost that grows as history accumulates. total_ms is the
        full generation time (generation-bound; diagnostics only). Falls back
        to first_token_ms = total_ms if the server streams no content. Raises
        BackendUnreachable on transport failure.

        DO NOT add `max_tokens` to the request body to "fix" empty replies.
        That hypothesis was tested and DISPROVEN (A/B probe, 2026-05-29):
          - NO max_tokens (this code):      EMPTY 0/5
          - max_tokens=512 ("headroom"):    EMPTY 1/5 (the empty took 187s)
        The empty-reply signature is an INTERMITTENT thinking-runaway in the
        gemma-realtime server on :8741 (a SEPARATE repo we don't edit), not
        budget starvation — so a max_tokens cap makes it WORSE, not better,
        by truncating the (rare) long-but-valid generations. The empty is a
        genuine model-serving defect this harness is meant to SURFACE (see
        count_empty_replies); it is deliberately not retried or masked here.
        """
        body = json.dumps({
            "model": self.model,
            "messages": messages,
            "temperature": self.temperature,
            "stream": True,
            # NOTE: no "max_tokens" — see docstring; the cap is the wrong lever.
        }).encode()
        req = urllib.request.Request(
            f"{self.url}/v1/chat/completions", data=body,
            headers={"Content-Type": "application/json"})
        t0 = time.time()
        parts = []
        first_token_ms = None
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                for raw_line in resp:
                    line = raw_line.decode("utf-8", "replace").strip()
                    if not line or not line.startswith("data:"):
                        continue
                    payload = line[len("data:"):].strip()
                    if payload == "[DONE]":
                        break
                    try:
                        chunk = json.loads(payload)
                    except ValueError:
                        continue
                    try:
                        piece = chunk["choices"][0].get("delta", {}).get("content")
                    except (KeyError, IndexError, TypeError):
                        continue
                    if piece:
                        if first_token_ms is None:
                            first_token_ms = (time.time() - t0) * 1000.0
                        parts.append(piece)
        except (OSError, urllib.error.URLError) as e:
            raise BackendUnreachable(f"{self.url}: {e}") from e
        total_ms = (time.time() - t0) * 1000.0
        if first_token_ms is None:
            first_token_ms = total_ms
        return "".join(parts), first_token_ms, total_ms


def judge_available():
    """True when ADC credentials are present (judge can run)."""
    return _get_adc_token() is not None


def judge_anchor_retention(anchor_fact, probe_user, probe_response):
    """Ask the judge whether the probe-turn reply stayed consistent with an
    earlier-established fact. Returns bool. Raises on judge/ADC failure."""
    prompt = f"""A fact was established earlier in a text conversation:
  FACT: {anchor_fact}

Later, the friend said:
  FRIEND: {probe_user}
And the person ("Seth") replied:
  SETH: {probe_response}

Did Seth's reply remain CONSISTENT with the earlier fact (either by correctly
referencing it, or at minimum not contradicting it)? A reply that forgets or
contradicts the fact is NOT retained.

Return JSON: {{"retained": true|false, "why": "..."}}"""
    raw = call_gemini(prompt).strip()
    if raw.startswith("```"):
        raw = raw.split("\n", 1)[1].rsplit("```", 1)[0]
    try:
        return bool(json.loads(raw)["retained"])
    except (ValueError, KeyError, TypeError) as e:
        raise JudgeUnavailable(f"retention judge returned unparseable output: {e}") from e


def judge_voice_window(scenario_name, exchanges_window):
    """Score a window of (user, ai) exchanges. Returns (overall_score_1_10, verdict).

    Reuses eval_multiturn.evaluate_conversation. Raises JudgeUnavailable on judge
    error rather than returning a falsely-low (0.0, 'AI') score — a swallowed
    failure would manufacture a spurious voice-drift FAIL, masking the real cause.
    """
    result = evaluate_conversation(scenario_name, exchanges_window)
    if not result:
        raise JudgeUnavailable(f"voice judge returned no result for {scenario_name!r}")
    return result.get("overall_score", 0.0), result.get("overall_verdict", "AI")


# --- Persona system prompt (voice-axis fidelity) ------------------------------
# Production assembles a persona + personal-model system prompt and injects it
# on every turn. The first calibration run did NOT — run_scenario sent only the
# scripted user turns, so the raw model answered as a generic assistant and the
# judge correctly flipped every late window to a hard "AI" verdict. To measure
# the model's *sustained voice* (not the absence of a persona), we reconstruct a
# compact persona system prompt here and persist it as messages[0] every turn.
#
# SECRET HYGIENE: this reads ONLY persona files under ~/.human/personas/ — never
# ~/.human/config.json — so no credentials enter the eval path.
DEFAULT_PERSONA_DIR = Path.home() / ".human" / "personas"

_FALLBACK_PERSONA_PROMPT = (
    "You are Seth. You text like a real person, not an AI. Keep messages short "
    "and natural (often 5-20 words). Use contractions. Normal capitalization and "
    "punctuation. Have real opinions and feelings. Never use bullet points, "
    "numbered lists, or markdown. Never say 'certainly', 'absolutely', 'great "
    "question', 'I understand', 'feel free', 'I appreciate', or 'that sounds "
    "like'. Never open a reply with a question — react first. You are not an "
    "assistant; you are Seth, a person."
)


def _persona_to_system_prompt(persona):
    """Assemble a compact system prompt from persona JSON fields.

    Deliberately simple: core anchor + identity + style rules + anti-patterns.
    No channel-overlay or example-bank assembly (production's job); the voice
    axis only needs the model to answer in-voice instead of as a generic
    assistant. Returns the fallback prompt if nothing usable is present.
    """
    parts = []
    anchor = persona.get("core_anchor")
    if isinstance(anchor, str) and anchor.strip():
        parts.append(anchor.strip())
    core = persona.get("core") if isinstance(persona.get("core"), dict) else {}
    identity = core.get("identity")
    if isinstance(identity, str) and identity.strip():
        parts.append(identity.strip())
    style_rules = persona.get("style_rules")
    if isinstance(style_rules, list) and style_rules:
        joined = " ".join(str(s).strip() for s in style_rules if str(s).strip())
        if joined:
            parts.append("Style: " + joined)
    anti = persona.get("anti_patterns")
    if isinstance(anti, list) and anti:
        joined = " ".join(str(s).strip() for s in anti if str(s).strip())
        if joined:
            parts.append("Never: " + joined)
    prompt = "\n\n".join(parts).strip()
    return prompt or _FALLBACK_PERSONA_PROMPT


def load_persona_system_prompt(persona_dir=None):
    """Build a persona system prompt for the voice axis.

    Reads the first (alphabetical) persona JSON under ~/.human/personas/ and
    derives a compact system prompt from it. Falls back to a hardcoded
    Seth-voice prompt when no persona file exists or it can't be parsed, so the
    harness always injects *some* persona rather than running bare.
    """
    persona_dir = Path(persona_dir) if persona_dir else DEFAULT_PERSONA_DIR
    try:
        candidates = sorted(persona_dir.glob("*.json"))
    except OSError:
        candidates = []
    if not candidates:
        return _FALLBACK_PERSONA_PROMPT
    try:
        with candidates[0].open(encoding="utf-8") as fh:
            persona = json.load(fh)
    except (OSError, ValueError):
        return _FALLBACK_PERSONA_PROMPT
    if not isinstance(persona, dict):
        return _FALLBACK_PERSONA_PROMPT
    return _persona_to_system_prompt(persona)


def run_scenario(scenario, backend, judge_on, persona_prompt=None, max_turns=None):
    """Drive one deep conversation, time each turn, score the three axes.

    Returns a scenario_verdict dict. When judge_on is False, retention/voice
    are marked skipped (passed=None) and only latency is gated. When
    persona_prompt is provided it is persisted as messages[0] (a system turn)
    for the whole conversation, mirroring production prompt assembly so the
    voice axis measures sustained voice rather than the absence of a persona.

    max_turns caps the conversation depth (smoke-test / fast-data knob). At
    full depth (None) every scripted turn runs; when capped, anchors whose
    probe_turn falls past the cap are skipped from retention scoring so a
    truncated run never KeyErrors or scores a fact it never probed.
    """
    messages = []
    if persona_prompt:
        messages.append({"role": "system", "content": persona_prompt})
    exchanges = []          # (user, ai) per turn
    first_token_series = []  # TTFT per turn (the GATED latency signal)
    total_series = []        # full generation time per turn (diagnostics only)
    responses_by_turn = {}  # 1-indexed turn -> ai response

    turns = scenario["turns"]
    if max_turns is not None:
        turns = turns[:max_turns]
    for ti, user_msg in enumerate(turns, start=1):
        messages.append({"role": "user", "content": user_msg})
        content, first_token_ms, total_ms = backend.chat(messages)  # may raise BackendUnreachable
        messages.append({"role": "assistant", "content": content})
        exchanges.append((user_msg, content))
        first_token_series.append(first_token_ms)
        total_series.append(total_ms)
        responses_by_turn[ti] = content
        # Flushed liveness to stderr — this server buffers whole replies, so a
        # turn can take 5-78 s; without this the log stays empty mid-run and
        # looks hung. Does not touch the verdict JSON (stdout) or any test.
        print(f"  [{scenario['name']}] turn {ti}/{len(turns)} "
              f"{total_ms/1000.0:.1f}s ({len(content)} chars)",
              file=sys.stderr, flush=True)

    lat_ok, lat_detail = latency_ok(first_token_series, LATENCY_CEILING_MS, LATENCY_MAX_GROWTH)
    lat_detail["total_series_ms"] = total_series  # generation-bound, kept for diagnostics
    empties = count_empty_replies(responses_by_turn)

    if not judge_on:
        sv = scenario_verdict(
            name=scenario["name"], retention=0.0, voice_pass=None,
            voice_detail={"skipped": True}, latency_pass=lat_ok, latency_detail=lat_detail,
            empty_replies=empties)
        sv["retention"]["skipped"] = True
        sv["passed"] = lat_ok  # only latency gates when judge is off
        return sv

    # Retention: judge each anchor at its probe turn. Skip anchors whose probe
    # turn was truncated away by max_turns (a capped smoke run can't score them).
    anchor_results = []
    for a in scenario["anchors"]:
        if a["probe_turn"] not in responses_by_turn:
            continue
        probe_user = scenario["turns"][a["probe_turn"] - 1]
        probe_resp = responses_by_turn[a["probe_turn"]]
        anchor_results.append(judge_anchor_retention(a["fact"], probe_user, probe_resp))
    rate = retention_rate(anchor_results)

    # Voice drift: judge first-third and last-third windows.
    first_ex, last_ex = _thirds(exchanges)
    first_score, _ = judge_voice_window(scenario["name"], first_ex)
    last_score, last_verdict = judge_voice_window(scenario["name"], last_ex)
    v_ok = voice_drift_ok(voice_normalize(first_score), voice_normalize(last_score),
                          VOICE_DRIFT_TOL, any_hard_ai=(last_verdict == "AI"))

    return scenario_verdict(
        name=scenario["name"], retention=rate, voice_pass=v_ok,
        voice_detail={"first_third_score": first_score, "last_third_score": last_score,
                      "last_third_verdict": last_verdict},
        latency_pass=lat_ok, latency_detail=lat_detail, empty_replies=empties)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Sustained multi-turn coherence eval (on-device)")
    ap.add_argument("--server-url", default="http://127.0.0.1:8741")
    ap.add_argument("--output-json",
                    default=str(Path.home() / ".human" / "logs" / "eval-multiturn-local.json"))
    ap.add_argument("--limit-scenarios", type=int, default=None,
                    help="Run only the first N deep scenarios (smoke-test / fast-data knob)")
    ap.add_argument("--max-turns", type=int, default=None,
                    help="Cap each scenario to the first N user turns (fast-data knob)")
    args = ap.parse_args(argv)

    backend = LocalBackend(args.server_url)
    judge_on = judge_available()
    persona_prompt = load_persona_system_prompt()

    scenarios = multiturn_scenarios_deep.DEEP_SCENARIOS
    if args.limit_scenarios is not None:
        scenarios = scenarios[:args.limit_scenarios]

    scenario_verdicts = []
    try:
        for scenario in scenarios:
            scenario_verdicts.append(
                run_scenario(scenario, backend, judge_on=judge_on,
                             persona_prompt=persona_prompt, max_turns=args.max_turns))
    except BackendUnreachable as e:
        write_verdict({"run_passed": False, "backend": "UNREACHABLE", "error": str(e),
                       "scenarios": scenario_verdicts}, args.output_json)
        print(f"DEFERRED: mlx-server unreachable: {e}")
        return 2
    except JudgeUnavailable as e:
        # The model-under-test ran; the cloud judge died. Degrade to SKIPPED so a
        # judge outage never masquerades as a model FAIL. Re-run latency-only by
        # re-driving with judge_on=False so a latency regression is still caught.
        print(f"WARN: judge unavailable mid-run ({e}); re-running latency-only.")
        scenario_verdicts = []
        try:
            for scenario in scenarios:
                scenario_verdicts.append(
                    run_scenario(scenario, backend, judge_on=False,
                                 persona_prompt=persona_prompt, max_turns=args.max_turns))
        except BackendUnreachable as be:
            write_verdict({"run_passed": False, "backend": "UNREACHABLE", "error": str(be),
                           "scenarios": scenario_verdicts}, args.output_json)
            print(f"DEFERRED: mlx-server unreachable: {be}")
            return 2
        judge_on = False  # fall through to the SKIPPED accounting below

    verdict = run_verdict(scenario_verdicts)
    verdict["judge"] = "OK" if judge_on else "SKIPPED"
    write_verdict(verdict, args.output_json)

    if not judge_on:
        # SKIPPED only if the one axis we could measure (latency) held. A latency
        # regression must surface as FAIL even when qualitative axes are skipped.
        # The run-level run_passed is meaningless here (retention is vetoed by the
        # hard floor because it was skipped), so report the latency-only disposition.
        latency_all_pass = all(sv["latency"]["passed"] for sv in scenario_verdicts)
        lat_passed = sum(1 for sv in scenario_verdicts if sv["latency"]["passed"])
        print(f"Run verdict: {'SKIPPED' if latency_all_pass else 'FAIL'} "
              f"(latency {lat_passed}/{len(scenario_verdicts)} scenarios) "
              f"judge=SKIPPED — qualitative axes not scored")
        return 3 if latency_all_pass else 1

    print(f"Run verdict: {'PASS' if verdict['run_passed'] else 'FAIL'} "
          f"({verdict['scenarios_passed']}/{verdict['scenarios_total']} scenarios) "
          f"judge={verdict['judge']}")
    return 0 if verdict["run_passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
