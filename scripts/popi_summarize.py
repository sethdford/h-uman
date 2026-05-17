#!/usr/bin/env python3
"""Sprint 11 / US-11.9 — POPI baseline CLI.

Reads recent (prompt, rejected, chosen) correction pairs from a source
(SQLite DB *or* JSONL fixture), runs the rule-based extractor in
`popi_extract.py`, and writes a <= 100-token plain-text preference
summary to a file (or stdout).

This is a baseline for the cloud-provider personalization comparison
in US-11.6 (held-out next-utterance log-likelihood eval). It is NOT
the full POPI architecture (arXiv 2510.17881) — that requires an
RL-trained inference model and is explicitly out of scope (see story
§Out of scope + design §1).

Determinism + no-network guarantees (AC-11.9.3):

  - No HTTP/HTTPS clients imported anywhere.
  - No randomness (deterministic ranking with stable tie-breaks).
  - No model weights loaded.
  - DB rows read in stable order (`ORDER BY created_at DESC LIMIT ?`).

Output contract (AC-11.9.1):

  - Whitespace-tokens of the summary <= --max-tokens (default 100).
  - Plain text on stdout or the file passed via --output.
  - Empty correction set (AC-11.9.4) -> empty string, exit code 0.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sqlite3
import sys
from pathlib import Path
from typing import Iterable, List, Optional, Sequence

# Local sibling module — load via importlib so this script also works when
# run directly via `python scripts/popi_summarize.py` outside of a package.
import importlib.util

_HERE = Path(__file__).resolve().parent
_EXTRACT_PATH = _HERE / "popi_extract.py"


def _load_extract_module():
    # Register in sys.modules BEFORE exec_module: the `@dataclass(frozen=True)`
    # decorators in popi_extract.py walk `sys.modules[cls.__module__]` to
    # resolve `KW_ONLY` sentinels (Python 3.14 dataclass internals). Without
    # the pre-registration the lookup returns None and crashes.
    spec = importlib.util.spec_from_file_location("popi_extract", _EXTRACT_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load popi_extract from {_EXTRACT_PATH}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["popi_extract"] = mod
    spec.loader.exec_module(mod)
    return mod


popi_extract = _load_extract_module()
CorrectionPair = popi_extract.CorrectionPair
summarize = popi_extract.summarize
DEFAULT_MAX_TOKENS = popi_extract.DEFAULT_MAX_TOKENS


# ── PII redaction (pure Python; mirrors src/security/pii.c patterns) ──────
#
# US-11.9 implementer brief AC-11.9.4 (per brief): "POPI summary is screened
# by the same PII redactor that protects dpo_miner.c outputs." The C-side
# `hu_pii_redact` covers emails, phone numbers, SSNs, IPs, credit-card-like
# digit runs. For the Python summarizer we mirror the same regex set —
# deterministic, no network, no shell-out — and call it on every loaded
# pair *before* it reaches the extractor.

_EMAIL_RE = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")
_PHONE_RE = re.compile(r"\b(?:\+?\d{1,3}[-.\s]?)?(?:\(?\d{3}\)?[-.\s]?)?\d{3}[-.\s]?\d{4}\b")
_SSN_RE = re.compile(r"\b\d{3}-\d{2}-\d{4}\b")
_IPV4_RE = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")
_CC_RE = re.compile(r"\b(?:\d[ -]?){13,19}\b")


def redact_pii(text: str) -> str:
    """Mirror of `hu_pii_redact` for Python-side inputs.

    Order matters: longest-match-first so CC numbers aren't shadowed by
    phone-number patterns.
    """
    text = _CC_RE.sub("[REDACTED_CC]", text)
    text = _SSN_RE.sub("[REDACTED_SSN]", text)
    text = _EMAIL_RE.sub("[REDACTED_EMAIL]", text)
    text = _PHONE_RE.sub("[REDACTED_PHONE]", text)
    text = _IPV4_RE.sub("[REDACTED_IP]", text)
    return text


def _redact_pair(pair: CorrectionPair) -> CorrectionPair:
    return CorrectionPair(
        prompt=redact_pii(pair.prompt),
        rejected=redact_pii(pair.rejected),
        chosen=redact_pii(pair.chosen),
    )


# ── Source loaders ────────────────────────────────────────────────────────


def load_pairs_from_sqlite(db_path: Path, max_pairs: int) -> List[CorrectionPair]:
    """Load up to `max_pairs` most-recent rows from `dpo_pairs.db`.

    Expected schema (see src/ml/dpo_miner.c):
        CREATE TABLE pairs (
            id INTEGER PRIMARY KEY,
            prompt TEXT NOT NULL,
            rejected TEXT NOT NULL,
            chosen TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            ...
        );

    The actual production schema has more columns; we read only the
    three we need. Missing table -> empty list (AC-11.9.4 cold-start
    behaviour).
    """
    if not db_path.exists():
        raise FileNotFoundError(f"corrections DB not found: {db_path}")
    pairs: List[CorrectionPair] = []
    conn = sqlite3.connect(str(db_path))
    try:
        cur = conn.cursor()
        try:
            cur.execute(
                "SELECT prompt, rejected, chosen FROM pairs "
                "ORDER BY created_at DESC LIMIT ?",
                (int(max_pairs),),
            )
        except sqlite3.OperationalError:
            # Table missing or schema mismatch -> cold-start path.
            return []
        for prompt, rejected, chosen in cur.fetchall():
            if not isinstance(prompt, str) or not isinstance(rejected, str) or not isinstance(chosen, str):
                continue
            if not (prompt.strip() and rejected.strip() and chosen.strip()):
                continue
            pairs.append(CorrectionPair(prompt=prompt, rejected=rejected, chosen=chosen))
    finally:
        conn.close()
    return pairs


def load_pairs_from_jsonl(path: Path, max_pairs: int) -> List[CorrectionPair]:
    """Load up to `max_pairs` rows from a JSONL fixture.

    Each row: {"prompt": str, "rejected": str, "chosen": str}.
    Rows missing any required field are skipped silently — fixtures
    are agent-owned, so a wrong-shape row is a fixture bug, not a
    runtime failure. Empty file -> empty list (AC-11.9.4).
    """
    if not path.exists():
        raise FileNotFoundError(f"corrections JSONL not found: {path}")
    pairs: List[CorrectionPair] = []
    with path.open("r", encoding="utf-8") as fp:
        for raw in fp:
            raw = raw.strip()
            if not raw:
                continue
            try:
                obj = json.loads(raw)
            except json.JSONDecodeError:
                continue
            if not isinstance(obj, dict):
                continue
            prompt = obj.get("prompt")
            rejected = obj.get("rejected")
            chosen = obj.get("chosen")
            if not (isinstance(prompt, str) and isinstance(rejected, str) and isinstance(chosen, str)):
                continue
            if not (prompt.strip() and rejected.strip() and chosen.strip()):
                continue
            pairs.append(CorrectionPair(prompt=prompt, rejected=rejected, chosen=chosen))
            if len(pairs) >= max_pairs:
                break
    return pairs


# ── Pipeline ─────────────────────────────────────────────────────────────


def run(
    corrections_path: Path,
    max_pairs: int,
    max_tokens: int,
    source_format: str,
) -> str:
    """End-to-end: load -> redact -> summarize. Returns the summary string."""
    if source_format == "sqlite":
        pairs = load_pairs_from_sqlite(corrections_path, max_pairs)
    elif source_format == "jsonl":
        pairs = load_pairs_from_jsonl(corrections_path, max_pairs)
    else:
        raise ValueError(f"unknown source format: {source_format}")
    redacted = [_redact_pair(p) for p in pairs]
    return summarize(redacted, max_tokens=max_tokens)


# ── CLI ──────────────────────────────────────────────────────────────────


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="popi_summarize.py",
        description=(
            "Sprint 11 / US-11.9 — POPI baseline: compress recent correction "
            "pairs into a <=100-token preference summary for system-prompt "
            "injection. Rule-based; no LLM call (per AC-11.9.3)."
        ),
    )
    p.add_argument(
        "--corrections-db",
        type=str,
        default=None,
        help="Path to SQLite DB (production: ~/.human/dpo_pairs.db).",
    )
    p.add_argument(
        "--corrections-jsonl",
        type=str,
        default=None,
        help="Path to JSONL fixture (testing/CI). Mutually exclusive with "
        "--corrections-db.",
    )
    p.add_argument(
        "--max-pairs",
        type=int,
        default=50,
        help="Maximum correction pairs to consume (default: 50).",
    )
    p.add_argument(
        "--max-tokens",
        type=int,
        default=DEFAULT_MAX_TOKENS,
        help=f"Maximum whitespace tokens in the output summary "
        f"(default: {DEFAULT_MAX_TOKENS}; AC-11.9.1).",
    )
    p.add_argument(
        "--output",
        type=str,
        default=None,
        help="Optional output file path. Default: stdout.",
    )
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    if args.corrections_db and args.corrections_jsonl:
        parser.error("--corrections-db and --corrections-jsonl are mutually exclusive")
    if not args.corrections_db and not args.corrections_jsonl:
        parser.error("must specify --corrections-db or --corrections-jsonl")

    if args.max_pairs <= 0:
        parser.error("--max-pairs must be positive")

    if args.corrections_db:
        source = "sqlite"
        path = Path(args.corrections_db).expanduser()
    else:
        source = "jsonl"
        path = Path(args.corrections_jsonl).expanduser()

    try:
        summary = run(
            corrections_path=path,
            max_pairs=args.max_pairs,
            max_tokens=args.max_tokens,
            source_format=source,
        )
    except (FileNotFoundError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    # Empty summary on cold-start is the AC-11.9.4 contract — write
    # nothing (file gets created and is empty) and exit 0.
    if args.output:
        Path(args.output).expanduser().write_text(summary, encoding="utf-8")
    else:
        # Don't add trailing newline if empty — the empty-cold-start
        # case must produce exactly "" so downstream callers can detect it.
        if summary:
            print(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
