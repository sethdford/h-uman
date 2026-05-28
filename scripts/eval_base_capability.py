#!/usr/bin/env python3
"""
Base-capability probe set + deterministic scorer (Dermot SOTA spec C2 / AC-3).

The longitudinal personalization gate needs an axis ORTHOGONAL to persona
fidelity: does continual fine-tuning preserve the model's *general* ability
(instruction-following, extraction, arithmetic)? This is the guard that turns
the 2026-05-25 scale=20 incident — where an adapter lifted Seth-voice but
destroyed instruction-following — into a gate FAIL instead of a shipped
regression (lora-scale-default-or-die.md).

Every probe is scored by a DETERMINISTIC checker (no LLM judge), so the axis is
cheap, reproducible, and free of judge drift across months of generations. A
probe scores 1.0 (pass) or 0.0 (fail); the base-capability score is the mean.

Checker types (in scripts/data/base-capability-probes.jsonl):
  json_keys     : response parses as JSON and contains all listed keys
  json_equals   : response parses as JSON and deep-equals the given value
  numeric_equals: the first number found in the response equals the value
  exact         : trimmed, case-folded response exactly equals the value
  regex         : re.search(pattern) matches (case-sensitive)
  regex_i       : re.search(pattern, IGNORECASE) matches

Usage:
  from eval_base_capability import score_base_capability, load_probes, probes_sha256
  per_probe, mean = score_base_capability(responses)   # responses align to probes
"""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

DEFAULT_PROBES = Path(__file__).parent / "data" / "base-capability-probes.jsonl"


def load_probes(path: Path | str = DEFAULT_PROBES) -> list[dict]:
    """Load the frozen probe set from JSONL. Order is significant: responses
    passed to score_base_capability must align to this order."""
    p = Path(path)
    probes: list[dict] = []
    with p.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                probes.append(json.loads(line))
    return probes


def probes_sha256(path: Path | str = DEFAULT_PROBES) -> str:
    """Stable hash of the probe-set file. Recorded in trajectory.json so a
    changed probe set loudly invalidates cross-generation comparison rather
    than silently shifting the base-capability baseline."""
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _first_number(text: str):
    """First integer/float in the text, or None. Tolerant of commas/$ etc."""
    m = re.search(r"-?\d[\d,]*\.?\d*", text)
    if not m:
        return None
    raw = m.group(0).replace(",", "")
    try:
        f = float(raw)
        return int(f) if f.is_integer() else f
    except ValueError:
        return None


def _strip_code_fence(text: str) -> str:
    """Drop a leading ```json / ``` fence if the model wrapped its answer."""
    t = text.strip()
    if t.startswith("```"):
        t = t.split("\n", 1)[1] if "\n" in t else t[3:]
        if t.rstrip().endswith("```"):
            t = t.rstrip()[:-3]
    return t.strip()


def _try_json(text: str):
    """Best-effort parse: whole string, then the first {...} or [...] span."""
    t = _strip_code_fence(text)
    try:
        return json.loads(t)
    except (json.JSONDecodeError, ValueError):
        pass
    for opener, closer in (("{", "}"), ("[", "]")):
        i, j = t.find(opener), t.rfind(closer)
        if 0 <= i < j:
            try:
                return json.loads(t[i : j + 1])
            except (json.JSONDecodeError, ValueError):
                continue
    return None


def check_probe(check: dict, response: str) -> bool:
    """Apply one probe's deterministic checker to a response. Returns pass/fail.
    Unknown checker types and error markers ([timeout]/[gen_err]/[empty]) fail
    closed (False)."""
    if response is None:
        return False
    resp = response.strip()
    if not resp or resp.startswith("["):  # [timeout] / [gen_err] / [empty]
        # Guard: a legitimate JSON-array answer also starts with "[". Only
        # treat known error markers as hard fails.
        if resp in ("[timeout]", "[empty]") or resp.startswith("[gen_err"):
            return False

    ctype = check.get("type")
    if ctype == "json_keys":
        obj = _try_json(resp)
        return isinstance(obj, dict) and all(k in obj for k in check["keys"])
    if ctype == "json_equals":
        return _try_json(resp) == check["value"]
    if ctype == "numeric_equals":
        return _first_number(resp) == check["value"]
    if ctype == "exact":
        return resp.casefold() == str(check["value"]).casefold()
    if ctype == "regex":
        return re.search(check["pattern"], resp) is not None
    if ctype == "regex_i":
        return re.search(check["pattern"], resp, re.IGNORECASE) is not None
    return False  # unknown checker → fail closed


def score_base_capability(
    responses: list[str], probes: list[dict] | None = None
) -> tuple[list[dict], float]:
    """Score a model's responses against the frozen probe set.

    Args:
        responses: model outputs, aligned 1:1 to `probes` order.
        probes: probe list; defaults to the frozen on-disk set.

    Returns:
        (per_probe, mean) where per_probe is a list of
        {"id", "passed"(bool), "response"} and mean is the pass-rate in [0, 1].
        Responses shorter than probes count the missing ones as failures.
    """
    probes = probes if probes is not None else load_probes()
    per_probe: list[dict] = []
    passed = 0
    for i, probe in enumerate(probes):
        resp = responses[i] if i < len(responses) else ""
        ok = check_probe(probe["check"], resp)
        if ok:
            passed += 1
        per_probe.append({"id": probe["id"], "passed": ok, "response": resp})
    mean = passed / len(probes) if probes else 0.0
    return per_probe, mean
