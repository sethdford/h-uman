#!/usr/bin/env python3
"""emotion_register.py — the emotional-register measurement axis.

Shared by scripts/measure_emotion_card.py (Seth's own texts -> emotion
card) and scripts/eval_emotion_register.py (the twin's sent replies vs the
card). Everything numeric lives here so the two can never disagree on a
definition — the same reason the style card reuses
eval_persona_evolution's feature functions.

The framework is Cowen & Keltner (2017): a fixed high-dimensional emotion
taxonomy, one label per message, and a comparison between DISTRIBUTIONS
rather than a single score. The judge is the local model on :8741, so no
message text leaves the machine, and its identity (model + taxonomy +
prompt hash) is written next to every number — numbers from different
judges are refused, not compared.

Refusal contract (.claude/rules/no-number-without-a-measurement.md):
MeasurementRefused / JudgeUnavailable propagate to the callers, which exit
non-zero and write nothing.
"""
import hashlib
import json
import math
import random
import re
import urllib.error
import urllib.request

TAXONOMY_VERSION = "cowen-keltner-27/v1"

# Cowen & Keltner, PNAS 2017 — the 27 distinct categories.
CATEGORIES = (
    "admiration",
    "adoration",
    "aesthetic appreciation",
    "amusement",
    "anxiety",
    "awe",
    "awkwardness",
    "boredom",
    "calmness",
    "confusion",
    "craving",
    "disgust",
    "empathic pain",
    "entrancement",
    "envy",
    "excitement",
    "fear",
    "horror",
    "interest",
    "joy",
    "nostalgia",
    "romance",
    "sadness",
    "satisfaction",
    "sexual desire",
    "sympathy",
    "triumph",
)
NEUTRAL = "neutral"  # matter-of-fact / logistical: most texts
LABELS = CATEGORIES + (NEUTRAL,)

# Fixed per-category valence so the valence number is DERIVED from the
# label, not judged twice. -1 negative, 0 mixed/ambiguous, +1 positive.
VALENCE = {
    "admiration": 1, "adoration": 1, "aesthetic appreciation": 1, "amusement": 1,
    "anxiety": -1, "awe": 1, "awkwardness": -1, "boredom": -1, "calmness": 1,
    "confusion": -1, "craving": 0, "disgust": -1, "empathic pain": -1,
    "entrancement": 1, "envy": -1, "excitement": 1, "fear": -1, "horror": -1,
    "interest": 1, "joy": 1, "nostalgia": 0, "romance": 1, "sadness": -1,
    "satisfaction": 1, "sexual desire": 1, "sympathy": 0, "triumph": 1,
    NEUTRAL: 0,
}

JUDGE_PROMPT = (
    "You are labeling the emotional register of ONE short text message the "
    "sender typed to someone they know. Pick the single emotion category the "
    "message most expresses from the SENDER's side (not the reader's), from "
    "exactly this list:\n{categories}\n"
    "If the message is matter-of-fact, logistical, or shows no feeling, use "
    "\"neutral\" with intensity 0.\n"
    "Intensity is how strongly the feeling shows in the wording, from 0.0 "
    "(none) to 1.0 (overwhelming). Plain texts are usually 0.0 to 0.3.\n"
    "Answer with ONLY a JSON object on one line: "
    "{{\"emotion\": \"<category>\", \"intensity\": <number>}}\n"
    "Message:\n<<<\n{text}\n>>>"
)
PROMPT_SHA = hashlib.sha256(JUDGE_PROMPT.encode("utf-8")).hexdigest()[:12]

DEFAULT_MLX_URL = "http://127.0.0.1:8741/v1"


class MeasurementRefused(Exception):
    """The inputs are not what the number would claim to measure."""


class JudgeUnavailable(Exception):
    """The local judge cannot be reached; defer, do not fabricate."""


def judge_id(model: str) -> str:
    return f"{model}|{TAXONOMY_VERSION}|{PROMPT_SHA}"


def render_prompt(text: str) -> str:
    cats = "\n".join(f"- {c}" for c in CATEGORIES)
    return JUDGE_PROMPT.format(categories=cats, text=text)


class LocalJudge:
    """OpenAI-compatible chat client for the local :8741 server.

    Same transport as eval_fidelity_live.generate_local; temperature 0 so
    the labels are as reproducible as the server allows."""

    def __init__(self, base_url: str = DEFAULT_MLX_URL, timeout: float = 60.0,
                 max_tokens: int = 80):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.max_tokens = max_tokens
        self._model = None

    def model(self) -> str:
        if self._model:
            return self._model
        try:
            req = urllib.request.Request(self.base_url + "/models", method="GET")
            with urllib.request.urlopen(req, timeout=5.0) as resp:  # noqa: S310 (localhost)
                payload = json.loads(resp.read().decode("utf-8"))
        except (urllib.error.URLError, OSError, ValueError) as e:
            raise JudgeUnavailable(f"{self.base_url}/models: {e}") from e
        data = payload.get("data") or []
        if not data or not data[0].get("id"):
            raise JudgeUnavailable(f"{self.base_url}/models listed no model")
        self._model = str(data[0]["id"])
        return self._model

    def id(self) -> str:
        return judge_id(self.model())

    def label(self, text: str) -> str:
        body = json.dumps({
            "model": self.model(),
            "messages": [{"role": "user", "content": render_prompt(text)}],
            "max_tokens": self.max_tokens,
            "temperature": 0,
        }).encode("utf-8")
        req = urllib.request.Request(
            self.base_url + "/chat/completions", data=body,
            headers={"Content-Type": "application/json"},
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:  # noqa: S310
                payload = json.loads(resp.read().decode("utf-8"))
        except (urllib.error.URLError, OSError, ValueError) as e:
            raise JudgeUnavailable(f"{self.base_url}/chat/completions: {e}") from e
        try:
            return payload["choices"][0]["message"]["content"] or ""
        except (KeyError, IndexError, TypeError):
            return ""


_THINK = re.compile(r"<think>.*?</think>", re.S)
_FENCE = re.compile(r"```(?:json)?", re.I)
_OBJ = re.compile(r"\{.*?\}", re.S)


def parse_label(raw):
    """Lenient parse of one judge reply -> (emotion, intensity) or None.

    Tolerates fenced JSON and <think> blocks (both observed from the local
    server); rejects anything outside the taxonomy so an off-list word can
    never become a category."""
    if not raw:
        return None
    s = _FENCE.sub("", _THINK.sub("", raw))
    m = _OBJ.search(s)
    if not m:
        return None
    try:
        obj = json.loads(m.group(0))
    except ValueError:
        return None
    if not isinstance(obj, dict):
        return None
    emotion = str(obj.get("emotion", "")).strip().lower()
    if emotion not in LABELS:
        return None
    try:
        intensity = float(obj.get("intensity", 0.0))
    except (TypeError, ValueError):
        return None
    if math.isnan(intensity):
        return None
    intensity = min(1.0, max(0.0, intensity))
    return (emotion, intensity)


def label_texts(judge, texts, progress=None):
    """Label every text; None where the judge's reply did not parse.

    A transport failure mid-run raises JudgeUnavailable — a half-labeled
    sample is not the sample the caller asked for."""
    out = []
    for i, text in enumerate(texts):
        out.append(parse_label(judge.label(text)))
        if progress and (i + 1) % 25 == 0:
            progress(i + 1, len(texts))
    return out


def _percentile_ci(xs):
    xs = sorted(xs)
    if not xs:
        return (0.0, 0.0)
    lo = xs[int(0.025 * (len(xs) - 1))]
    hi = xs[int(0.975 * (len(xs) - 1))]
    return (lo, hi)


def _stats(sample):
    n = len(sample)
    neutral = sum(1 for e, _ in sample if e == NEUTRAL) / n
    intensity = sum(i for _, i in sample) / n
    valence = sum(VALENCE[e] for e, _ in sample) / n
    return (neutral, intensity, valence)


def aggregate(labels, n_resamples: int = 2000, seed: int = 42) -> dict:
    """Distribution over LABELS plus neutral share, mean intensity and
    valence with bootstrap 95% CIs. Refuses an empty sample."""
    valid = [lab for lab in labels if lab]
    n = len(valid)
    failures = len(labels) - n
    if n == 0:
        raise MeasurementRefused("no message received a valid label")
    counts = {lab: 0 for lab in LABELS}
    for e, _ in valid:
        counts[e] += 1
    point = _stats(valid)
    rng = random.Random(seed)
    boots = ([], [], [])
    for _ in range(n_resamples):
        s = [valid[rng.randrange(n)] for _ in range(n)]
        for k, v in enumerate(_stats(s)):
            boots[k].append(v)
    cis = [_percentile_ci(b) for b in boots]

    def axis(k):
        return {"value": point[k], "ci_lo": cis[k][0], "ci_hi": cis[k][1], "n": n}

    top = sorted(((lab, counts[lab] / n) for lab in CATEGORIES if counts[lab] > 0),
                 key=lambda t: (-t[1], t[0]))[:4]
    return {
        "n": n,
        "parse_failures": failures,
        "distribution": {lab: {"count": counts[lab], "share": counts[lab] / n} for lab in LABELS},
        "neutral_share": axis(0),
        "mean_intensity": axis(1),
        "valence_mean": axis(2),
        "top": [{"emotion": lab, "share": share} for lab, share in top],
    }


def _vec(dist: dict):
    return [float(dist[lab]["share"]) if lab in dist else 0.0 for lab in LABELS]


def jsd(p_dist: dict, q_dist: dict) -> float:
    """Base-2 Jensen-Shannon divergence between two LABELS distributions.
    0 = identical, 1 = disjoint support. Symmetric, no smoothing needed."""
    p, q = _vec(p_dist), _vec(q_dist)
    m = [(a + b) / 2.0 for a, b in zip(p, q)]

    def kl(a, b):
        return sum(x * math.log2(x / y) for x, y in zip(a, b) if x > 0)

    return 0.5 * kl(p, m) + 0.5 * kl(q, m)


def check_judge_match(card: dict, judge_identity: str) -> None:
    """The card's numbers and tonight's numbers must come from the same
    judge (model + taxonomy + prompt). Otherwise the divergence measures
    the judges, not the twin."""
    card_id = ((card.get("judge") or {}).get("id")) or ""
    if card_id != judge_identity:
        raise MeasurementRefused(
            f"judge mismatch: card was labeled by '{card_id}', tonight's judge is "
            f"'{judge_identity}' — rebuild the card with scripts/measure_emotion_card.py"
        )


def compare(card: dict, twin_labels, n_resamples: int = 2000, seed: int = 42) -> dict:
    """Twin distribution vs the card: JSD with a bootstrap CI over the twin
    sample, axis deltas (twin - card) and the largest per-category shifts."""
    twin = aggregate(twin_labels, n_resamples=n_resamples, seed=seed)
    card_dist = card["distribution"]
    point = jsd(twin["distribution"], card_dist)
    valid = [lab for lab in twin_labels if lab]
    n = len(valid)
    rng = random.Random(seed)
    boots = []
    for _ in range(n_resamples):
        s = [valid[rng.randrange(n)] for _ in range(n)]
        counts = {lab: 0 for lab in LABELS}
        for e, _ in s:
            counts[e] += 1
        boots.append(jsd({lab: {"share": counts[lab] / n} for lab in LABELS}, card_dist))
    # Basic (reverse-percentile) bootstrap, clipped to JSD's range. The
    # plain percentile interval sits ABOVE the point estimate here because
    # resampling a finite sample always adds divergence (an identical twin
    # measured 0.000 with a percentile CI of [0.001, 0.033]); reflecting
    # the resample distribution around the point removes that bias.
    p_lo, p_hi = _percentile_ci(boots)
    lo = min(1.0, max(0.0, 2.0 * point - p_hi))
    hi = min(1.0, max(0.0, 2.0 * point - p_lo))
    per_cat = []
    for lab in LABELS:
        t = twin["distribution"][lab]["share"]
        c = float(card_dist.get(lab, {}).get("share", 0.0))
        per_cat.append({"emotion": lab, "twin": t, "card": c, "delta": t - c})
    per_cat.sort(key=lambda d: (-abs(d["delta"]), d["emotion"]))

    def delta(axis):
        return twin[axis]["value"] - float(card[axis]["value"])

    return {
        "twin": twin,
        "jsd": {"value": point, "ci_lo": lo, "ci_hi": hi, "n": n},
        "deltas": {
            "neutral_share": delta("neutral_share"),
            "mean_intensity": delta("mean_intensity"),
            "valence_mean": delta("valence_mean"),
        },
        "largest_shifts": per_cat[:5],
    }
