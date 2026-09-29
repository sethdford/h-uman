#!/usr/bin/env python3
"""Delete unreferenced C functions (definition + header prototype) in bulk.

PURPOSE
    Companion to scripts/dev/delete-modules.sh, which deletes whole modules.
    This script deletes INDIVIDUAL functions from files that survive: for each
    (symbol, defining file) pair it removes

      1. the function definition in the .c file, from the start of its return
         type through the brace that matches its opening `{`, plus the
         doc-comment block (block `/* */` or a run of `//` lines) that
         immediately precedes it with no blank line in between;
      2. the prototype for the same symbol in any header under include/ or in
         the defining file's own directory, together with that prototype's
         immediately-preceding doc comment.

    Braces, parentheses and the `;`/`{` disambiguation are counted on a MASKED
    copy of the file in which every character inside a string literal, char
    literal, line comment or block comment has been replaced by a space. That
    is what makes `snprintf(buf, n, "}")` and `/* hu_foo(void); */` invisible
    to the scanner: only real code is ever matched, so a mention of the symbol
    inside a comment is neither matched nor removed.

    Anything the script cannot find unambiguously is left untouched and
    printed, so it can be handled by hand.

USAGE
    scripts/dev/delete-functions.py LIST.tsv [--dry-run] [--root DIR]

    LIST.tsv is TAB-separated, one `symbol<TAB>path/to/defining/file.c` per
    line; blank lines and `#` comments are ignored. Paths are relative to
    --root (default: the repository root inferred from this script's location).

    --dry-run reports what would be removed without writing any file.

EXIT CODES
    0  every symbol in the list had its definition removed (prototypes that do
       not exist are reported but are not an error -- some functions are
       header-less by design)
    1  at least one definition could not be found or was ambiguous
    2  usage error (bad arguments, unreadable list, missing file)
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# --------------------------------------------------------------------------
# Masking: blank out strings, chars and comments so only code is scanned.
# --------------------------------------------------------------------------


def mask_code(text: str) -> str:
    """Return a same-length copy of *text* with non-code characters blanked.

    Characters inside string literals, character literals, line comments and
    block comments become spaces. Newlines are preserved everywhere so that
    offsets and line numbers map 1:1 onto the original text.
    """
    out = list(text)
    i = 0
    n = len(text)
    NORMAL, LINE_COMMENT, BLOCK_COMMENT, STRING, CHAR = range(5)
    state = NORMAL
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == NORMAL:
            if c == "/" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = LINE_COMMENT
                continue
            if c == "/" and nxt == "*":
                out[i] = out[i + 1] = " "
                i += 2
                state = BLOCK_COMMENT
                continue
            if c == '"':
                out[i] = " "
                i += 1
                state = STRING
                continue
            if c == "'":
                out[i] = " "
                i += 1
                state = CHAR
                continue
            i += 1
            continue
        if state == LINE_COMMENT:
            if c == "\n":
                state = NORMAL
                i += 1
                continue
            # A backslash-newline continues a line comment onto the next line.
            if c == "\\" and nxt == "\n":
                out[i] = " "
                i += 2
                continue
            out[i] = " "
            i += 1
            continue
        if state == BLOCK_COMMENT:
            if c == "*" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = NORMAL
                continue
            if c != "\n":
                out[i] = " "
            i += 1
            continue
        # STRING or CHAR
        terminator = '"' if state == STRING else "'"
        if c == "\\" and i + 1 < n:
            out[i] = " "
            if text[i + 1] != "\n":
                out[i + 1] = " "
            i += 2
            continue
        if c == terminator:
            out[i] = " "
            i += 1
            state = NORMAL
            continue
        if c != "\n":
            out[i] = " "
        i += 1
    return "".join(out)


def match_paren(masked: str, open_idx: int) -> int | None:
    """Index of the `)` matching the `(` at *open_idx*, or None."""
    depth = 0
    for i in range(open_idx, len(masked)):
        c = masked[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
    return None


def match_brace(masked: str, open_idx: int) -> int | None:
    """Index of the `}` matching the `{` at *open_idx*, or None."""
    depth = 0
    for i in range(open_idx, len(masked)):
        c = masked[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
    return None


def next_code_char(masked: str, idx: int) -> tuple[int, str] | tuple[None, None]:
    """First non-whitespace code character at or after *idx*."""
    for i in range(idx, len(masked)):
        if not masked[i].isspace():
            return i, masked[i]
    return None, None


# --------------------------------------------------------------------------
# Line bookkeeping
# --------------------------------------------------------------------------


def line_starts(text: str) -> list[int]:
    starts = [0]
    for i, c in enumerate(text):
        if c == "\n":
            starts.append(i + 1)
    return starts


def line_of(starts: list[int], offset: int) -> int:
    """0-based line index containing *offset* (binary search)."""
    lo, hi = 0, len(starts) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if starts[mid] <= offset:
            lo = mid
        else:
            hi = mid - 1
    return lo


def doc_comment_start(lines: list[str], first: int) -> int:
    """Extend *first* backwards over the doc comment immediately above it.

    Returns the first line index of the doc-comment block, or *first* when the
    definition/prototype is not preceded by one. A blank line between the
    comment and the code means the comment is not a doc comment for it.
    """
    if first == 0:
        return first
    prev = lines[first - 1].strip()
    if not prev:
        return first
    if prev.startswith("#"):
        # A preprocessor line is never a doc comment, even when it carries a
        # trailing one: `#else /* !HU_ENABLE_SQLITE */` must survive.
        return first
    if prev.endswith("*/"):
        # `/* ... */` closed on the same line it opened: that one line is it.
        body = prev[:-2]
        if "/*" in body:
            return first - 1
        # Otherwise walk up to the line that opens the block comment, giving
        # up the moment we hit something that cannot be inside a comment.
        i = first - 2
        while i >= 0:
            stripped = lines[i].strip()
            if stripped.startswith("/*"):
                return i
            if (
                not stripped
                or stripped.startswith("#")
                or stripped.endswith(";")
                or stripped.endswith("{")
                or stripped.endswith("}")
                or stripped.endswith("*/")
            ):
                return first
            i -= 1
        return first
    if prev.startswith("//"):
        i = first - 1
        while i >= 0 and lines[i].lstrip().startswith("//"):
            i -= 1
        return i + 1
    return first


# --------------------------------------------------------------------------
# Locating definitions and prototypes
# --------------------------------------------------------------------------


@dataclass
class Span:
    path: Path
    first: int  # inclusive 0-based line
    last: int  # inclusive 0-based line
    kind: str  # "definition" or "prototype"


def occurrences(masked: str, symbol: str) -> list[int]:
    """Offsets of `symbol` followed by `(` in code (not comments/strings)."""
    return [m.start() for m in re.finditer(r"\b" + re.escape(symbol) + r"\s*\(", masked)]


def find_definition(text: str, masked: str, symbol: str) -> tuple[list[Span], list[str]]:
    lines = text.split("\n")
    starts = line_starts(text)
    found: list[Span] = []
    notes: list[str] = []
    for off in occurrences(masked, symbol):
        open_paren = masked.index("(", off + len(symbol))
        close_paren = match_paren(masked, open_paren)
        if close_paren is None:
            continue
        idx, ch = next_code_char(masked, close_paren + 1)
        if ch != "{":
            continue  # a call or a prototype, not a definition
        ln = line_of(starts, off)
        if lines[ln].lstrip().startswith("#"):
            continue  # macro definition
        # Definition start: the symbol's own line, or the previous line when
        # the symbol sits at column 0 and the return type is above it.
        first = ln
        col = off - starts[ln]
        if col == 0 and ln > 0 and lines[ln - 1].strip():
            first = ln - 1
        first = doc_comment_start(lines, first)
        close_brace = match_brace(masked, idx)
        if close_brace is None:
            notes.append(f"unbalanced braces after {symbol}")
            continue
        last = line_of(starts, close_brace)
        found.append(Span(Path(), first, last, "definition"))
    return found, notes


def find_prototypes(text: str, masked: str, symbol: str) -> list[Span]:
    lines = text.split("\n")
    starts = line_starts(text)
    found: list[Span] = []
    for off in occurrences(masked, symbol):
        open_paren = masked.index("(", off + len(symbol))
        close_paren = match_paren(masked, open_paren)
        if close_paren is None:
            continue
        idx, ch = next_code_char(masked, close_paren + 1)
        if ch != ";":
            continue
        ln = line_of(starts, off)
        if lines[ln].lstrip().startswith("#"):
            continue
        first = ln
        col = off - starts[ln]
        if col == 0 and ln > 0 and lines[ln - 1].strip():
            first = ln - 1
        first = doc_comment_start(lines, first)
        last = line_of(starts, idx)
        found.append(Span(Path(), first, last, "prototype"))
    return found


# --------------------------------------------------------------------------
# Removal
# --------------------------------------------------------------------------


def remove_spans(text: str, spans: list[Span]) -> str:
    """Remove the given line ranges, collapsing the blank line each leaves."""
    lines = text.split("\n")
    drop: set[int] = set()
    for s in spans:
        drop.update(range(s.first, s.last + 1))
    # Blank-line repair is decided on the MERGED set, not per span: two
    # adjacent definitions removed back to back are one hole, and treating
    # them separately leaves a double blank line behind.
    for i in sorted(drop):
        if i - 1 in drop:
            continue  # not the top of a hole
        end = i
        while end + 1 in drop:
            end += 1
        before_blank = i == 0 or not lines[i - 1].strip()
        after = end + 1
        if before_blank and after < len(lines) and not lines[after].strip():
            drop.add(after)
    return "\n".join(line for i, line in enumerate(lines) if i not in drop)


@dataclass
class Result:
    removed_defs: list[str] = field(default_factory=list)
    removed_protos: list[tuple[str, str]] = field(default_factory=list)
    no_proto: list[str] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)


def header_candidates(root: Path, src: Path) -> list[Path]:
    heads = sorted((root / "include").rglob("*.h"))
    heads += sorted(p for p in (root / src.parent).glob("*.h"))
    seen: set[Path] = set()
    unique = []
    for h in heads:
        if h not in seen:
            seen.add(h)
            unique.append(h)
    return unique


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("list", help="TSV of symbol<TAB>defining-file")
    ap.add_argument("--root", default=None, help="repository root")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument(
        "--all-variants",
        action="store_true",
        help="remove EVERY definition of the symbol in the file instead of "
        "refusing when there is more than one. Use only after checking by hand "
        "that the copies are #if/#else conditional-compilation variants.",
    )
    args = ap.parse_args()

    root = Path(args.root) if args.root else Path(__file__).resolve().parents[2]
    try:
        rows = [
            ln.split("\t")
            for ln in Path(args.list).read_text().splitlines()
            if ln.strip() and not ln.startswith("#")
        ]
    except OSError as exc:
        print(f"error: cannot read list: {exc}", file=sys.stderr)
        return 2
    if any(len(r) != 2 for r in rows):
        print("error: every row must be symbol<TAB>file", file=sys.stderr)
        return 2

    res = Result()

    # Group by source file so each file is parsed and rewritten once.
    by_src: dict[str, list[str]] = {}
    order: list[str] = []
    for symbol, src in rows:
        by_src.setdefault(src, []).append(symbol)
        if src not in order:
            order.append(src)

    # Headers are shared across sources; accumulate edits and write at the end.
    header_spans: dict[Path, list[Span]] = {}

    for src in order:
        path = root / src
        if not path.is_file():
            for symbol in by_src[src]:
                res.problems.append(f"{symbol}: defining file {src} does not exist")
            continue
        text = path.read_text()
        masked = mask_code(text)
        spans: list[Span] = []
        for symbol in by_src[src]:
            found, notes = find_definition(text, masked, symbol)
            for n in notes:
                res.problems.append(f"{symbol}: {n}")
            if not found:
                res.problems.append(f"{symbol}: no definition found in {src}")
                continue
            if len(found) > 1 and not args.all_variants:
                res.problems.append(
                    f"{symbol}: {len(found)} definitions in {src} "
                    f"(lines {', '.join(str(s.first + 1) for s in found)}) -- skipped"
                )
                continue
            if len(found) > 1:
                print(
                    f"note: {symbol}: removing all {len(found)} conditional "
                    f"variants in {src}"
                )
            spans.extend(found)
            res.removed_defs.append(f"{symbol}\t{src}")
        if spans and not args.dry_run:
            path.write_text(remove_spans(text, spans))

    # Prototypes: only for symbols whose definition was actually removed.
    done = set(res.removed_defs)
    cache: dict[str, list[Path]] = {}
    for symbol, src in rows:
        if f"{symbol}\t{src}" not in done:
            continue
        if src not in cache:
            cache[src] = header_candidates(root, Path(src))
        hits = 0
        for h in cache[src]:
            htext = h.read_text()
            if symbol not in htext:
                continue
            hspans = find_prototypes(htext, mask_code(htext), symbol)
            if not hspans:
                continue
            hits += len(hspans)
            header_spans.setdefault(h, []).extend(hspans)
            res.removed_protos.append((symbol, str(h.relative_to(root))))
        if hits == 0:
            res.no_proto.append(symbol)

    if not args.dry_run:
        for h, hspans in header_spans.items():
            htext = h.read_text()
            h.write_text(remove_spans(htext, hspans))

    # ---------------------------------------------------------------- report
    print(f"definitions removed : {len(res.removed_defs)} / {len(rows)}")
    print(f"prototypes removed  : {len(res.removed_protos)}")
    if res.no_proto:
        print(f"no prototype found  : {len(res.no_proto)}")
        for s in res.no_proto:
            print(f"    {s}")
    if res.problems:
        print(f"NOT HANDLED         : {len(res.problems)}")
        for p in res.problems:
            print(f"    {p}")
    if args.dry_run:
        print("(dry run -- nothing written)")
    return 1 if res.problems else 0


if __name__ == "__main__":
    sys.exit(main())
