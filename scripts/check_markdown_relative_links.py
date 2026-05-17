#!/usr/bin/env python3
"""
Scan Markdown under configured repo roots for relative links and verify targets exist.
Checks: inline [text](url), reference-style [id]: url definitions, and HTML <a href="...">.

Skips http(s)://, mailto:, javascript:, data:, vscode:, and pure #anchors.

Environment:
  MARKDOWN_LINK_ROOTS — space-separated top-level dirs under repo root (default below).
  MARKDOWN_LINK_SCAN_ALL=1 — scan every *.md under repo except known junk dirs.
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

# [text](url) or [text](url "title")
INLINE_LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)(?:\s+[\"'][^\"']*[\"'])?\)")
# Link reference definition: [label]: url — same line only ([ \t]+ avoids crossing newlines into fenced blocks)
REF_DEF_RE = re.compile(r"^\s{0,3}\[[^\]]+\]:[ \t]+<?([^>\s]+)>?", re.MULTILINE)
HTML_HREF_RE = re.compile(r'(?i)\bhref\s*=\s*["\']([^"\']+)["\']')
EXT_IN_PATH = re.compile(r"\.(md|markdown|html?|svg|png|jpe?g|gif|webp|json|ya?ml|txt|pdf)\b", re.I)

ROOT = Path(__file__).resolve().parent.parent

DEFAULT_ROOTS = ("docs", "human-skills", "skill-registry")

JUNK_DIR_NAMES = frozenset(
    {
        ".git",
        "node_modules",
        "build",
        "build-arm64",
        "build-check",
        "build-minimal",
        "dist",
        ".cache",
        "__pycache__",
        ".pytest_cache",
        "DerivedData",
    }
)


def junk_path(path: Path) -> bool:
    try:
        rel = path.relative_to(ROOT)
    except ValueError:
        return True
    parts = rel.parts
    for i, part in enumerate(parts):
        if part in JUNK_DIR_NAMES:
            return True
        if i >= 2 and parts[i - 2] == "plugins" and parts[i - 1] == "cache":
            return True
    return False


def collect_md_files() -> list[Path]:
    scan_all = os.environ.get("MARKDOWN_LINK_SCAN_ALL", "").strip() == "1"
    files: set[Path] = set()

    if scan_all:
        for p in ROOT.rglob("*.md"):
            if p.is_file() and not junk_path(p):
                files.add(p.resolve())
    else:
        roots_env = os.environ.get("MARKDOWN_LINK_ROOTS", "").strip()
        if roots_env:
            names = [x.strip() for x in roots_env.split() if x.strip()]
        else:
            names = list(DEFAULT_ROOTS)
        for name in names:
            d = ROOT / name
            if d.is_dir():
                for p in d.rglob("*.md"):
                    if p.is_file():
                        files.add(p.resolve())
        for p in ROOT.glob("*.md"):
            if p.is_file():
                files.add(p.resolve())

    return sorted(files)


def ref_def_destination_plausible(url: str) -> bool:
    """Avoid false positives on prose like '[Source: …]: User said …'."""
    u = url.strip()
    if not u or skip_href(u):
        return False
    if u.startswith((".", "/", "\\")):
        return True
    if "/" in u or "\\" in u:
        return True
    if EXT_IN_PATH.search(u):
        return True
    if u.startswith("#"):
        return False
    return False


def skip_href(raw: str) -> bool:
    s = raw.strip()
    if not s or s.startswith("#"):
        return True
    lowered = s.lower()
    if lowered.startswith(
        ("http://", "https://", "mailto:", "javascript:", "data:", "vscode:", "file:")
    ):
        return True
    return False


_LINE_SUFFIX_RE = re.compile(r":\d+(?:-\d+)?(?:,\d+(?:-\d+)?)*$")


def _strip_editor_line_suffix(path_part: str) -> str:
    """Strip ":<line>", ":<start>-<end>", or ":<l1>,<l2>" suffixes used by the
    standard editor convention to deep-link to a file location (e.g.
    `src/daemon.c:975`, `src/foo.c:100-110`, `src/bar.c:1,5`). These are
    semantic locators handled by every modern editor and IDE; the file on
    disk has no `:digit` extension. Without this stripping, the link
    checker rejects perfectly valid file-and-line refs and forces docs to
    drop useful information. Standardized 2026-05-17 alongside the
    ci-rot cleanup — same audit pass that produced docs/research/
    2026-05-16-proactive-audit/findings.md, where the convention is used
    extensively. """
    return _LINE_SUFFIX_RE.sub("", path_part)


def check_path(raw: str, parent: Path) -> str | None:
    """Return failure reason, or None if OK / skipped.

    Resolution order (2026-05-17): try the URL as parent-relative first
    (Markdown's default), then fall back to repo-root-relative if it
    doesn't start with `./`, `../`, or `/`. The fallback covers a common
    convention in audit/research docs that write `src/daemon.c` as a
    repo-root path the way every code-review tool, `gh`, and editor
    `:line` link interprets it. Without this, every audit doc that
    references source files has to use brittle `../../../src/...`
    paths that break the moment the doc is moved. """
    raw = raw.strip()
    if skip_href(raw):
        return None
    if raw.startswith("<") and raw.endswith(">"):
        raw = raw[1:-1]
    path_part = raw.split("#", 1)[0]
    if not path_part:
        return None
    path_part = _strip_editor_line_suffix(path_part)
    if not path_part:
        return None

    candidates: list[Path] = []
    parent_relative = (parent / path_part).resolve()
    candidates.append(parent_relative)

    # If the URL doesn't look explicitly parent-relative (no leading dot
    # or slash), also try resolving from repo root. Order matters — we
    # prefer the explicit relative-to-parent interpretation when both
    # files happen to exist.
    if not path_part.startswith(("./", "../", "/")):
        repo_root_relative = (ROOT / path_part).resolve()
        candidates.append(repo_root_relative)

    in_repo = False
    for cand in candidates:
        try:
            cand.relative_to(ROOT)
            in_repo = True
        except ValueError:
            continue
        if cand.exists():
            return None
    if not in_repo:
        return "escapes repo root"
    return "missing"


def extract_urls_from_file(text: str) -> list[str]:
    urls: list[str] = []
    for m in INLINE_LINK_RE.finditer(text):
        urls.append(m.group(1).strip())
    for m in REF_DEF_RE.finditer(text):
        u = m.group(1).strip()
        if ref_def_destination_plausible(u):
            urls.append(u)
    for m in HTML_HREF_RE.finditer(text):
        urls.append(m.group(1).strip())
    return urls


def main() -> int:
    md_files = collect_md_files()
    if not md_files:
        print("ERROR: no Markdown files matched", file=sys.stderr)
        return 1

    broken: list[tuple[str, str, str]] = []

    for md_path in md_files:
        try:
            text = md_path.read_text(encoding="utf-8")
        except OSError as e:
            print(f"WARN: could not read {md_path}: {e}", file=sys.stderr)
            continue
        parent = md_path.parent
        rel_src = str(md_path.relative_to(ROOT))
        for raw in extract_urls_from_file(text):
            reason = check_path(raw, parent)
            if reason:
                broken.append((rel_src, raw, reason))

    if broken:
        print("Broken or invalid relative Markdown / HTML links:\n")
        for src, href, reason in broken:
            print(f"  {src}")
            print(f"    -> {href} ({reason})")
        print(f"\nTotal: {len(broken)}")
        return 1

    roots_note = "MARKDOWN_LINK_SCAN_ALL=1" if os.environ.get("MARKDOWN_LINK_SCAN_ALL") == "1" else f"roots {DEFAULT_ROOTS} + *.md at repo root"
    print(f"OK: relative links resolved for {len(md_files)} Markdown file(s) ({roots_note}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
