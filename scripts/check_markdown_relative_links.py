#!/usr/bin/env python3
"""
Scan Markdown under configured repo roots for relative links and verify targets exist.
Checks: inline [text](url), reference-style [id]: url definitions, and HTML <a href="...">.

Skips http(s)://, mailto:, javascript:, data:, vscode:, and pure #anchors.

Environment:
  MARKDOWN_LINK_ROOTS — space-separated top-level dirs under repo root (default below).
  MARKDOWN_LINK_SCAN_ALL=1 — scan every *.md under repo except known junk dirs
    and this checker's own fixtures (tests/fixtures/check-markdown-links/).

Inline code spans and fenced code blocks are blanked before extraction, so a
`](` inside code is never treated as a link.
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
    if rel.as_posix().startswith(FIXTURE_DIR + "/"):
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


def check_path(raw: str, parent: Path) -> str | None:
    """Return failure reason, or None if OK / skipped."""
    raw = raw.strip()
    if skip_href(raw):
        return None
    if raw.startswith("<") and raw.endswith(">"):
        raw = raw[1:-1]
    path_part = raw.split("#", 1)[0]
    if not path_part:
        return None
    # Strip source-line/column anchors like "src/foo.c:42" or "foo.c:42:7"
    # — common markdown convention for pointing at a specific code site.
    # Without this strip, the checker treats the entire "path:line" string
    # as a literal filename and reports "missing" on every line-anchored
    # source-code reference in the repo's plan docs.
    if ":" in path_part:
        head = path_part.split(":", 1)[0]
        # Only strip when what follows the colon is a line/column number
        # (digits, optionally :col-digits). Anything else stays as-is so
        # we don't accidentally accept URLs that genuinely contain colons.
        rest = path_part[len(head) + 1 :]
        if rest and all(seg.isdigit() for seg in rest.split(":") if seg):
            path_part = head
    target = (parent / path_part).resolve()
    try:
        target.relative_to(ROOT)
    except ValueError:
        return "escapes repo root"
    if not target.exists():
        return "missing"
    return None


# Fenced code block delimiters (CommonMark §4.5): up to 3 spaces of indent,
# a run of 3+ backticks or tildes, optional info string. A backtick fence's
# info string may not itself contain a backtick.
FENCE_OPEN_RE = re.compile(r"^ {0,3}(`{3,}|~{3,})(.*)$")
# Inline code span (CommonMark §6.1): the closing backtick run must be exactly
# as long as the opening run and neither may be flanked by another backtick.
INLINE_CODE_RE = re.compile(r"(?<!`)(`+)(?!`)(.+?)(?<!`)\1(?!`)", re.DOTALL)
FIXTURE_DIR = "tests/fixtures/check-markdown-links"


def _blank(s: str) -> str:
    """Replace every non-newline character with a space (keeps line count)."""
    return re.sub(r"[^\n]", " ", s)


def strip_fenced_blocks(text: str) -> str:
    lines = text.split("\n")
    open_char = ""
    open_len = 0
    for i, line in enumerate(lines):
        if not open_char:
            m = FENCE_OPEN_RE.match(line)
            if m and not (m.group(1)[0] == "`" and "`" in m.group(2)):
                open_char, open_len = m.group(1)[0], len(m.group(1))
                lines[i] = _blank(line)
            continue
        stripped = line.lstrip(" ")
        indent = len(line) - len(stripped)
        run = len(stripped) - len(stripped.lstrip(open_char))
        if indent <= 3 and run >= open_len and stripped[run:].strip() == "":
            open_char = ""
        lines[i] = _blank(line)  # an unclosed fence runs to end of document
    return "\n".join(lines)


def strip_code(text: str) -> str:
    """Blank out fenced blocks and inline code spans before link extraction.

    Code is not prose: a regex such as `['’](t\\|re\\|ve)` in a spec table
    (docs/plans/2026-09-02-persona-evolution/spec.md, 2026-09-03) is exactly
    the shape of a Markdown link and was reported as a missing file
    "t\\|re\\|ve\\|ll\\|d\\|s\\|m". Stripped text is replaced with whitespace
    of the same line count, so REF_DEF_RE's ^-anchored matching and any
    line-number reporting stay correct. Fixtures:
    tests/fixtures/check-markdown-links/run-smoke-test.sh."""
    text = strip_fenced_blocks(text)
    return INLINE_CODE_RE.sub(lambda m: _blank(m.group(0)), text)


def extract_urls_from_file(text: str) -> list[str]:
    urls: list[str] = []
    text = strip_code(text)
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
