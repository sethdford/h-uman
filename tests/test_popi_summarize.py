"""Sprint 11 / US-11.9 — `scripts/popi_summarize.py` test suite.

Covers the AC for the POPI rule-based summarizer:

  AC-11.9.1  Summary <= 100 whitespace tokens; >= 3 distinct preferences
             captured from a fixture evidencing multiple categories.
  AC-11.9.3  No real LLM/network calls under HU_IS_TEST.
  AC-11.9.4  Cold-start (0 correction pairs) -> empty string, exit 0.

Plus defensive guards:

  - Determinism: 10 successive runs produce byte-identical output.
  - Category classifier unit tests (length / formality / directness /
    emoji / markdown).
  - PII redaction integration (emails, phones, IPs scrubbed before
    pairs reach the extractor).
  - Sprint 8 broken-adapter regression: a pad-heavy "rejected" set
    should NOT trick POPI into emitting a pad-favouring summary.

All tests are deterministic, offline, and require no network or model
weights. They run with bare `pytest` and Python stdlib only.
"""
from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import subprocess
import sys
from typing import List

import pytest


_HERE = pathlib.Path(__file__).resolve().parent
_REPO = _HERE.parent
_SCRIPTS = _REPO / "scripts"
_SUMMARIZE = _SCRIPTS / "popi_summarize.py"
_EXTRACT = _SCRIPTS / "popi_extract.py"
_FIX = _HERE / "fixtures"

_SYNTH = _FIX / "popi_corrections_synth.jsonl"
_EMPTY = _FIX / "popi_corrections_empty.jsonl"
_PII = _FIX / "popi_corrections_pii.jsonl"


# ── Module loader ─────────────────────────────────────────────────────────


def _load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


popi_extract = _load("popi_extract", _EXTRACT)
popi_summarize = _load("popi_summarize", _SUMMARIZE)


CorrectionPair = popi_extract.CorrectionPair


# ── AC-11.9.1: token budget + >= 3 categories ────────────────────────────


def test_summary_under_token_limit():
    """Summary must be <= 100 whitespace tokens (AC-11.9.1)."""
    summary = popi_summarize.run(
        corrections_path=_SYNTH,
        max_pairs=50,
        max_tokens=100,
        source_format="jsonl",
    )
    assert summary, "non-empty fixture must produce a non-empty summary"
    assert len(summary.split()) <= 100, (
        f"summary has {len(summary.split())} tokens, over the 100-token budget: {summary!r}"
    )


def test_summary_captures_three_distinct_preferences():
    """Synth fixture evidences all 5 categories; summary must include >= 3.

    Per AC-11.9.1 the summary must capture "at least 3 distinct style
    preferences evidenced in the corrections".
    """
    summary = popi_summarize.run(
        corrections_path=_SYNTH,
        max_pairs=50,
        max_tokens=100,
        source_format="jsonl",
    )
    categories_present = sum(
        1 for cat in popi_extract.CATEGORY_ORDER if f"{cat}:" in summary
    )
    assert categories_present >= 3, (
        f"summary captured only {categories_present} categories: {summary!r}"
    )


def test_max_tokens_strict_enforcement_at_emit_time():
    """Even with absurdly small budget, output stays in-budget or empty.

    Token-count enforcement must be structural, not best-effort. If the
    cheapest clause would already overflow, the summarizer returns the
    empty string rather than violate the contract.
    """
    # Smallest plausible budget that still fits the prefix + one clause.
    for budget in (1, 2, 3, 5, 10):
        summary = popi_summarize.run(
            corrections_path=_SYNTH,
            max_pairs=50,
            max_tokens=budget,
            source_format="jsonl",
        )
        if summary:
            assert len(summary.split()) <= budget, (
                f"budget={budget}, got {len(summary.split())} tokens: {summary!r}"
            )


# ── AC-11.9.4: cold-start empty fixture ──────────────────────────────────


def test_empty_corrections_returns_empty():
    """0-pair fixture -> empty string, exit 0 (AC-11.9.4)."""
    summary = popi_summarize.run(
        corrections_path=_EMPTY,
        max_pairs=50,
        max_tokens=100,
        source_format="jsonl",
    )
    assert summary == "", f"cold-start must return empty string, got: {summary!r}"


def test_cold_start_cli_exits_zero(tmp_path):
    """CLI on empty corrections returns exit 0 with no stdout."""
    out_file = tmp_path / "summary.txt"
    rc = subprocess.run(
        [
            sys.executable,
            str(_SUMMARIZE),
            "--corrections-jsonl",
            str(_EMPTY),
            "--max-pairs",
            "50",
            "--max-tokens",
            "100",
            "--output",
            str(out_file),
        ],
        capture_output=True,
        check=False,
    )
    assert rc.returncode == 0, (rc.returncode, rc.stderr.decode())
    assert out_file.read_text() == ""


# ── AC-11.9.3: no network under HU_IS_TEST ───────────────────────────────


def test_no_network_under_hu_is_test(monkeypatch):
    """No real LLM / HTTP calls anywhere in the summarizer (AC-11.9.3).

    We monkeypatch urllib, http.client, and socket.socket to raise on any
    use, then run the full pipeline. If anything reaches for the network
    the test trips.
    """
    monkeypatch.setenv("HU_IS_TEST", "1")

    def boom(*a, **k):
        raise RuntimeError("network call attempted under HU_IS_TEST")

    import socket as _socket
    import urllib.request as _ureq
    import http.client as _hc

    monkeypatch.setattr(_socket, "socket", boom, raising=True)
    monkeypatch.setattr(_ureq, "urlopen", boom, raising=True)
    monkeypatch.setattr(_hc, "HTTPConnection", boom, raising=True)
    monkeypatch.setattr(_hc, "HTTPSConnection", boom, raising=True)

    summary = popi_summarize.run(
        corrections_path=_SYNTH,
        max_pairs=50,
        max_tokens=100,
        source_format="jsonl",
    )
    assert summary


def test_summarizer_imports_no_http_libs():
    """Defensive: popi_summarize.py must not import requests / httpx.

    A future contributor adding an LLM call would need to import a HTTP
    client. Guarding the import surface keeps AC-11.9.3 honest.
    """
    src = _SUMMARIZE.read_text()
    forbidden = ("import requests", "import httpx", "from requests", "from httpx")
    for needle in forbidden:
        assert needle not in src, (
            f"popi_summarize.py imports {needle!r} — would break AC-11.9.3"
        )


# ── Determinism ──────────────────────────────────────────────────────────


def test_summary_deterministic_across_runs():
    """Identical input -> byte-identical summary across 10 runs."""
    outputs = []
    for _ in range(10):
        outputs.append(
            popi_summarize.run(
                corrections_path=_SYNTH,
                max_pairs=50,
                max_tokens=100,
                source_format="jsonl",
            )
        )
    assert len(set(outputs)) == 1, f"non-deterministic: {sorted(set(outputs))}"


# ── Category classifier unit tests ───────────────────────────────────────


def _pair(prompt: str, rejected: str, chosen: str) -> CorrectionPair:
    return CorrectionPair(prompt=prompt, rejected=rejected, chosen=chosen)


def test_length_vote_detects_shorter_chosen():
    p = _pair(
        "q",
        "This is a much longer rejected response that goes on and on and on with extra words.",
        "short",
    )
    votes = popi_extract.classify_pair(p)
    assert any(v.category == "length" and v.direction == "shorter" for v in votes)


def test_length_vote_detects_longer_chosen():
    p = _pair(
        "q",
        "short",
        "This is a much longer chosen response that goes on and on and on with extra words.",
    )
    votes = popi_extract.classify_pair(p)
    assert any(v.category == "length" and v.direction == "longer" for v in votes)


def test_formality_vote_detects_casual():
    p = _pair("q", "I will respond shortly.", "yeah lol on it")
    votes = popi_extract.classify_pair(p)
    assert any(
        v.category == "formality" and v.direction == "more casual" for v in votes
    )


def test_directness_vote_detects_hedge_removal():
    p = _pair("q", "Maybe we could perhaps consider it.", "Yes, do it.")
    votes = popi_extract.classify_pair(p)
    assert any(
        v.category == "directness" and v.direction == "less hedged" for v in votes
    )


def test_emoji_vote_detects_addition():
    p = _pair("q", "great", "great 🎉")
    votes = popi_extract.classify_pair(p)
    assert any(v.category == "emoji" and v.direction == "uses emoji" for v in votes)


def test_markdown_vote_detects_addition():
    p = _pair("q", "three things to do", "- thing one\n- thing two\n- thing three")
    votes = popi_extract.classify_pair(p)
    assert any(
        v.category == "markdown" and v.direction == "uses markdown" for v in votes
    )


# ── Aggregation + ranking ────────────────────────────────────────────────


def test_rank_categories_orders_by_evidence_count():
    """Categories with more votes outrank ones with fewer."""
    pairs = [
        _pair("q", "longer text" * 20, "tiny"),  # length: shorter
        _pair("q", "longer text" * 20, "tiny"),  # length: shorter
        _pair("q", "great", "great 🎉"),  # emoji
    ]
    hist = popi_extract.aggregate_votes(pairs)
    ranked = popi_extract.rank_categories(hist)
    # First entry must be "length" (2 votes) ahead of "emoji" (1 vote).
    assert ranked[0][0] == "length"


def test_aggregate_empty_returns_empty():
    hist = popi_extract.aggregate_votes([])
    assert hist == {}


# ── PII redaction integration ────────────────────────────────────────────


def test_pii_redactor_scrubs_email():
    out = popi_summarize.redact_pii("contact me at seth.ford@example.com please")
    assert "seth.ford@example.com" not in out
    assert "[REDACTED_EMAIL]" in out


def test_pii_redactor_scrubs_phone():
    out = popi_summarize.redact_pii("call 555-123-4567 anytime")
    assert "555-123-4567" not in out
    assert "[REDACTED_PHONE]" in out


def test_pii_redactor_scrubs_ip():
    out = popi_summarize.redact_pii("server is at 192.168.1.100 today")
    assert "192.168.1.100" not in out
    assert "[REDACTED_IP]" in out


def test_pii_fixture_redacted_before_extractor(tmp_path):
    """PII-bearing fixture: extractor never sees raw email/phone/IP.

    We instrument the extractor by wrapping its summarize() to capture
    the redacted pairs that flow into it.
    """
    captured: List[CorrectionPair] = []
    original_summarize = popi_extract.summarize

    def spy(pairs, max_tokens=100, top_k=5):
        captured.extend(pairs)
        return original_summarize(pairs, max_tokens=max_tokens, top_k=top_k)

    # Re-bind on the summarize-module level (where popi_summarize.run looks it up).
    saved = popi_summarize.summarize
    popi_summarize.summarize = spy
    try:
        popi_summarize.run(
            corrections_path=_PII,
            max_pairs=50,
            max_tokens=100,
            source_format="jsonl",
        )
    finally:
        popi_summarize.summarize = saved

    blob = " ".join(p.prompt + p.rejected + p.chosen for p in captured)
    assert "seth.ford@example.com" not in blob
    assert "555-123-4567" not in blob
    assert "192.168.1.100" not in blob


# ── Sprint 8 regression guard: pad-leakage doesn't trick POPI ────────────


def test_pad_leaked_rejected_does_not_drive_summary():
    """If rejected texts contain `<pad>` tokens, POPI must NOT emit a
    summary that recommends pad usage. Specifically: the summary must
    not contain the string '<pad>' or 'pad', because pad-heaviness is
    a corruption signal from Sprint 8 (broken adapter), not a user
    style preference.

    This is a defensive baseline test — the rule-based extractor has no
    pad-detection rule, but if a future contributor adds a "verbatim
    rejected token" heuristic this guard catches the regression.
    """
    # Fabricate pairs that look like a pad-leaked adapter's training
    # data (Sprint 8 SMOKE_RUN_NOTES.md): rejected text padded out with
    # `<pad>` tokens, chosen text clean.
    pairs = [
        _pair(
            "anything",
            "<pad> <pad> <pad> <pad> <pad> answer <pad> <pad> <pad>",
            "answer",
        )
        for _ in range(30)
    ]
    summary = popi_extract.summarize(pairs, max_tokens=100)
    assert "<pad>" not in summary
    assert " pad " not in f" {summary} "


def test_summary_never_outperforms_coherent_lora_silently():
    """Defensive: POPI must not produce a summary so verbose that it
    smothers the system-prompt budget downstream. The 100-token
    enforcement is the structural answer to "did POPI cheat by spilling
    text into the persona block?".
    """
    summary = popi_extract.summarize(
        [_pair("q", "long" * 50, "short")] * 20, max_tokens=100
    )
    # 100 tokens is the upper bound. The whole summary as we emit it is
    # well below 50 in practice; assert that we leave room for a
    # downstream persona block (>= 30 tokens of slack).
    assert len(summary.split()) < 70, (
        f"summary too verbose ({len(summary.split())} tokens): {summary!r}"
    )


# ── CLI integration ──────────────────────────────────────────────────────


def test_cli_writes_summary_to_output_file(tmp_path):
    out = tmp_path / "summary.txt"
    rc = subprocess.run(
        [
            sys.executable,
            str(_SUMMARIZE),
            "--corrections-jsonl",
            str(_SYNTH),
            "--max-pairs",
            "50",
            "--max-tokens",
            "100",
            "--output",
            str(out),
        ],
        capture_output=True,
        check=False,
    )
    assert rc.returncode == 0, (rc.returncode, rc.stderr.decode())
    body = out.read_text()
    assert body.startswith("User style:")
    assert len(body.split()) <= 100


def test_cli_requires_source():
    """Must pass --corrections-db or --corrections-jsonl."""
    rc = subprocess.run(
        [sys.executable, str(_SUMMARIZE), "--max-pairs", "50"],
        capture_output=True,
        check=False,
    )
    assert rc.returncode != 0
    assert b"corrections" in rc.stderr.lower()


def test_cli_rejects_both_sources(tmp_path):
    """--corrections-db and --corrections-jsonl are mutually exclusive."""
    db = tmp_path / "x.db"
    db.write_bytes(b"")
    rc = subprocess.run(
        [
            sys.executable,
            str(_SUMMARIZE),
            "--corrections-db",
            str(db),
            "--corrections-jsonl",
            str(_SYNTH),
        ],
        capture_output=True,
        check=False,
    )
    assert rc.returncode != 0


def test_cli_rejects_invalid_max_pairs():
    rc = subprocess.run(
        [
            sys.executable,
            str(_SUMMARIZE),
            "--corrections-jsonl",
            str(_SYNTH),
            "--max-pairs",
            "0",
        ],
        capture_output=True,
        check=False,
    )
    assert rc.returncode != 0


# ── SQLite loader ────────────────────────────────────────────────────────


def test_sqlite_loader_reads_recent_pairs(tmp_path):
    """SQLite source format reads up to max_pairs rows ordered DESC."""
    import sqlite3

    db = tmp_path / "dpo.db"
    conn = sqlite3.connect(str(db))
    cur = conn.cursor()
    cur.execute(
        "CREATE TABLE pairs (id INTEGER PRIMARY KEY, prompt TEXT, "
        "rejected TEXT, chosen TEXT, created_at INTEGER)"
    )
    rows = [
        (1, "q1", "I will respond promptly with full details.", "ok", 1000),
        (2, "q2", "Maybe we should consider it perhaps.", "Do it.", 2000),
        (3, "q3", "great", "great 🎉", 3000),
    ]
    cur.executemany(
        "INSERT INTO pairs (id, prompt, rejected, chosen, created_at) "
        "VALUES (?, ?, ?, ?, ?)",
        rows,
    )
    conn.commit()
    conn.close()

    summary = popi_summarize.run(
        corrections_path=db,
        max_pairs=10,
        max_tokens=100,
        source_format="sqlite",
    )
    assert summary
    assert "User style:" in summary


def test_sqlite_loader_missing_table_cold_starts(tmp_path):
    """Missing table -> cold-start (empty summary), not an error."""
    import sqlite3

    db = tmp_path / "empty.db"
    sqlite3.connect(str(db)).close()  # create empty DB, no table
    summary = popi_summarize.run(
        corrections_path=db,
        max_pairs=10,
        max_tokens=100,
        source_format="sqlite",
    )
    assert summary == ""


def test_sqlite_loader_missing_file_raises(tmp_path):
    """Missing DB file -> FileNotFoundError surfaced to the CLI."""
    with pytest.raises(FileNotFoundError):
        popi_summarize.run(
            corrections_path=tmp_path / "no-such.db",
            max_pairs=10,
            max_tokens=100,
            source_format="sqlite",
        )


# ── Emit-time edge cases ─────────────────────────────────────────────────


def test_emit_summary_zero_budget_raises():
    with pytest.raises(ValueError):
        popi_extract.emit_summary([("length", "shorter", 1)], max_tokens=0)


def test_emit_summary_over_ceiling_raises():
    with pytest.raises(ValueError):
        popi_extract.emit_summary(
            [("length", "shorter", 1)],
            max_tokens=popi_extract.MAX_TOKENS_HARD_CEILING + 1,
        )


def test_emit_summary_empty_ranked_returns_empty():
    assert popi_extract.emit_summary([], max_tokens=100) == ""
