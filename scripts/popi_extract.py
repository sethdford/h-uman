#!/usr/bin/env python3
"""Sprint 11 / US-11.9 — POPI rule-based correction-pair extractor (pure).

Pure functions for the POPI (Personalization Of Prompts by Inference,
arXiv 2510.17881) rule-based baseline. The full POPI architecture trains
an RL inference model on heterogeneous user signals; this baseline does
the cheapest deterministic version:

  - Consume (prompt, rejected, chosen) correction pairs
  - Compute per-pair edit-deltas across 5 categories
    (length, formality, directness, emoji, markdown)
  - Rank categories by evidence count
  - Emit a fixed-form, whitespace-token-budgeted summary

No randomness, no network, no LLM calls. Deterministic by construction:
identical input -> byte-identical output.

Why "rule-based, not LLM-call": US-11.9 design §1. Token-budget metric:
US-11.9 AC-11.9.1 (whitespace-split tokens; len(s.split()) <= 100).
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple


# ── Constants ─────────────────────────────────────────────────────────────

# Default whitespace-token budget per AC-11.9.1.
DEFAULT_MAX_TOKENS = 100

# Configurable upper bound for exploration (Risk 1 mitigation in design §4).
# Not the default; production CLI defaults to DEFAULT_MAX_TOKENS=100.
MAX_TOKENS_HARD_CEILING = 200

# Length-delta threshold above which a pair "votes" for the length category.
# Per design §1: chosen shorter/longer than rejected by >= 30%.
_LENGTH_DELTA_THRESHOLD = 0.30

# Hedge tokens whose removal counts as a directness signal.
_HEDGE_TOKENS = frozenset({
    "maybe", "perhaps", "possibly", "kinda", "sort", "sorta", "somewhat",
    "i think", "i guess", "i suppose", "it seems", "might", "could be",
})

# Casual/informal markers whose presence in chosen (or absence in rejected)
# votes for the formality=casual category.
_INFORMAL_MARKERS = frozenset({
    "lol", "tbh", "imo", "btw", "yeah", "ya", "yep", "nope", "ok", "k",
    "gonna", "wanna", "gotta", "kinda", "lemme", "dunno",
})

# Naive emoji detector: any codepoint in the BMP supplementary symbols / pictographs
# ranges. We do not import `regex` or `emoji` — keep stdlib-only.
_EMOJI_RE = re.compile(
    "[\U0001F300-\U0001FAFF"
    "\U00002600-\U000027BF"
    "\U0001F600-\U0001F64F"
    "\U0001F900-\U0001F9FF"
    "]",
    flags=re.UNICODE,
)

# Markdown structural markers.
_BULLET_RE = re.compile(r"(?m)^\s*[-*+]\s+\S")
_HEADING_RE = re.compile(r"(?m)^\s*#{1,6}\s+\S")
_CODE_FENCE_RE = re.compile(r"```")

# Category names — stable order for deterministic ranking on ties.
# When two categories tie on evidence count, the lower-index name wins.
# This is the only place ordering is enforced; everywhere else uses dicts
# (Python 3.7+ preserves insertion order).
CATEGORY_ORDER: Tuple[str, ...] = (
    "length",
    "formality",
    "directness",
    "emoji",
    "markdown",
)


# ── Data classes ──────────────────────────────────────────────────────────


@dataclass(frozen=True)
class CorrectionPair:
    """One (prompt, rejected, chosen) row from `dpo_pairs.db`.

    Frozen so it can be a dict key and so test fixtures cannot be mutated
    by accident mid-test.
    """

    prompt: str
    rejected: str
    chosen: str


@dataclass(frozen=True)
class CategoryVote:
    """One pair's contribution to one category.

    `direction` is a category-specific human-readable string (e.g.
    "shorter", "more casual", "less hedged", "uses emoji",
    "less markdown"). Aggregation groups by (category, direction) and
    picks the dominant direction per category in `rank_categories`.
    """

    category: str
    direction: str


# ── Per-pair classifiers (pure functions) ─────────────────────────────────


def _length_vote(pair: CorrectionPair) -> Optional[CategoryVote]:
    rej_n = max(len(pair.rejected), 1)
    cho_n = max(len(pair.chosen), 1)
    ratio = cho_n / rej_n
    if ratio <= (1.0 - _LENGTH_DELTA_THRESHOLD):
        return CategoryVote("length", "shorter")
    if ratio >= (1.0 + _LENGTH_DELTA_THRESHOLD):
        return CategoryVote("length", "longer")
    return None


def _has_marker(text: str, markers) -> bool:
    """Word-boundary marker check, case-insensitive."""
    lo = text.lower()
    for m in markers:
        # Use simple substring with whitespace boundaries — adequate for
        # short markers and chat-style text.
        if f" {m} " in f" {lo} " or lo == m:
            return True
    return False


def _formality_vote(pair: CorrectionPair) -> Optional[CategoryVote]:
    rej_informal = _has_marker(pair.rejected, _INFORMAL_MARKERS)
    cho_informal = _has_marker(pair.chosen, _INFORMAL_MARKERS)
    if cho_informal and not rej_informal:
        return CategoryVote("formality", "more casual")
    if rej_informal and not cho_informal:
        return CategoryVote("formality", "more formal")
    # Contraction signal: "i'll" / "you're" / "don't" in chosen but not rejected.
    rej_contract = bool(re.search(r"\b\w+'\w+\b", pair.rejected))
    cho_contract = bool(re.search(r"\b\w+'\w+\b", pair.chosen))
    if cho_contract and not rej_contract:
        return CategoryVote("formality", "more casual")
    if rej_contract and not cho_contract:
        return CategoryVote("formality", "more formal")
    return None


def _directness_vote(pair: CorrectionPair) -> Optional[CategoryVote]:
    rej_hedged = _has_marker(pair.rejected, _HEDGE_TOKENS)
    cho_hedged = _has_marker(pair.chosen, _HEDGE_TOKENS)
    if rej_hedged and not cho_hedged:
        return CategoryVote("directness", "less hedged")
    if cho_hedged and not rej_hedged:
        return CategoryVote("directness", "more hedged")
    return None


def _emoji_vote(pair: CorrectionPair) -> Optional[CategoryVote]:
    rej_emoji = bool(_EMOJI_RE.search(pair.rejected))
    cho_emoji = bool(_EMOJI_RE.search(pair.chosen))
    if cho_emoji and not rej_emoji:
        return CategoryVote("emoji", "uses emoji")
    if rej_emoji and not cho_emoji:
        return CategoryVote("emoji", "avoids emoji")
    return None


def _markdown_vote(pair: CorrectionPair) -> Optional[CategoryVote]:
    def md_score(text: str) -> int:
        return (
            (1 if _BULLET_RE.search(text) else 0)
            + (1 if _HEADING_RE.search(text) else 0)
            + (1 if _CODE_FENCE_RE.search(text) else 0)
        )

    rej_md = md_score(pair.rejected)
    cho_md = md_score(pair.chosen)
    if cho_md > rej_md:
        return CategoryVote("markdown", "uses markdown")
    if rej_md > cho_md:
        return CategoryVote("markdown", "less markdown")
    return None


_CLASSIFIERS = (
    _length_vote,
    _formality_vote,
    _directness_vote,
    _emoji_vote,
    _markdown_vote,
)


def classify_pair(pair: CorrectionPair) -> List[CategoryVote]:
    """Return all category votes a single pair contributes.

    A pair may vote for 0..5 categories (one per classifier). Order in
    the returned list is the order of `_CLASSIFIERS` — deterministic.
    """
    votes: List[CategoryVote] = []
    for cls in _CLASSIFIERS:
        v = cls(pair)
        if v is not None:
            votes.append(v)
    return votes


# ── Aggregation + ranking ─────────────────────────────────────────────────


def aggregate_votes(pairs: Sequence[CorrectionPair]) -> Dict[str, Dict[str, int]]:
    """Build a {category -> {direction -> count}} histogram across pairs.

    Empty input -> empty dict (no votes). This is what `cold_start` returns
    after — AC-11.9.4 path.
    """
    hist: Dict[str, Dict[str, int]] = {c: {} for c in CATEGORY_ORDER}
    for pair in pairs:
        for vote in classify_pair(pair):
            hist[vote.category][vote.direction] = (
                hist[vote.category].get(vote.direction, 0) + 1
            )
    # Drop empty categories so callers can iterate easily.
    return {c: d for c, d in hist.items() if d}


def rank_categories(
    hist: Dict[str, Dict[str, int]], top_k: int = 5
) -> List[Tuple[str, str, int]]:
    """Rank (category, dominant_direction, count) tuples.

    Tie-breaks:
      1. Higher total count wins.
      2. Lower CATEGORY_ORDER index wins (stable, deterministic).
      3. Lower direction name (lexicographic) wins (also deterministic).
    """
    ranked: List[Tuple[str, str, int]] = []
    for category in CATEGORY_ORDER:
        directions = hist.get(category)
        if not directions:
            continue
        # Dominant direction within this category: max count, tie-break by name.
        best_direction = min(
            directions.keys(),
            key=lambda d: (-directions[d], d),
        )
        total = sum(directions.values())
        ranked.append((category, best_direction, total))

    # Sort across categories: -count first, then CATEGORY_ORDER index.
    cat_index = {c: i for i, c in enumerate(CATEGORY_ORDER)}
    ranked.sort(key=lambda t: (-t[2], cat_index[t[0]]))
    return ranked[:top_k]


# ── Template emission ────────────────────────────────────────────────────


def emit_summary(
    ranked: Sequence[Tuple[str, str, int]],
    max_tokens: int = DEFAULT_MAX_TOKENS,
) -> str:
    """Render a fixed-form summary, budgeted to <= max_tokens whitespace tokens.

    Format (per design §1, item 5):
        User style: prefers <X>; avoids <Y>; tends to <Z>.

    Concretely we emit `User style: <cat1=dir1>; <cat2=dir2>; ...` and
    truncate by clause if over budget. Empty input -> empty string
    (AC-11.9.4).

    The budget enforcement is at emission time. If the longest single
    clause exceeds the budget on its own, we return the empty string
    rather than violate the contract — token-count enforcement is
    structural, not best-effort.
    """
    if max_tokens <= 0 or max_tokens > MAX_TOKENS_HARD_CEILING:
        raise ValueError(
            f"max_tokens must be in (0, {MAX_TOKENS_HARD_CEILING}], got {max_tokens}"
        )
    if not ranked:
        return ""

    # Build clauses in ranked order.
    clauses: List[str] = []
    for category, direction, _count in ranked:
        clauses.append(f"{category}: {direction}")

    # Greedy emit while staying within budget.
    prefix = "User style:"
    out = prefix
    appended = 0
    for i, clause in enumerate(clauses):
        sep = " " if i == 0 else "; "
        candidate = out + sep + clause + ("." if i == len(clauses) - 1 else "")
        # Token-count includes the final period only if this is the last clause.
        # Compare on the candidate _without_ trailing punctuation since
        # split() doesn't see it as its own token.
        if len(candidate.split()) > max_tokens:
            break
        out = out + sep + clause
        appended += 1

    if appended == 0:
        return ""

    out = out + "."
    # Final hard guard — must hold structurally.
    if len(out.split()) > max_tokens:
        return ""
    return out


# ── Pipeline ─────────────────────────────────────────────────────────────


def summarize(
    pairs: Sequence[CorrectionPair],
    max_tokens: int = DEFAULT_MAX_TOKENS,
    top_k: int = 5,
) -> str:
    """End-to-end: pairs -> ranked categories -> token-budgeted summary."""
    hist = aggregate_votes(pairs)
    ranked = rank_categories(hist, top_k=top_k)
    return emit_summary(ranked, max_tokens=max_tokens)
