#!/usr/bin/env python3
"""Cut one anchored block of lines out of a C source file.

Used by the hu_agent_turn carve
(docs/superpowers/plans/2026-09-30-agent-turn-carve-phase1.md). Stage moves are
verbatim: the block leaves agent_turn.c byte for byte and lands in a stage
file. Copying 100-1,500 lines by hand is where "verbatim" stops being true, so
the cut is mechanical and every anchor must match exactly one line.

  carve-block.py --file F --start S [--start-offset N] --end-after E
                 [--match exact|prefix] [--drop L]... [--drop-range FIRST LAST]...
                 --block-out B --replace-with R [--dry-run]

--start / --end-after  the block's first line and the first line AFTER it
--match                exact (default): whole line incl. indentation;
                       prefix: the line starts with the given text
--start-offset N       shift the start N lines from the --start match
                       (-1 = include the line above a unique anchor)
--drop / --drop-range  lines inside the block that must stay in F: removed
                       from the block and substituted, in order, for the line
                       "@@CARVE_DROPPED@@" in R. Dropping without that marker
                       is an error, so no line can be lost silently.
--block-out B          receives the cut block (minus dropped lines)
--replace-with R       its lines replace the block in F
Exit 0 on success, 2 on any anchor or usage error (nothing is written).
"""
import argparse
import pathlib
import sys

MARKER = "@@CARVE_DROPPED@@"


def die(msg):
    print(f"carve-block: {msg}", file=sys.stderr)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser(description="Cut one anchored block out of a C file.")
    ap.add_argument("--file", required=True)
    ap.add_argument("--start", required=True)
    ap.add_argument("--start-offset", type=int, default=0)
    ap.add_argument("--end-after", required=True)
    ap.add_argument("--match", choices=("exact", "prefix"), default="exact")
    ap.add_argument("--drop", action="append", default=[])
    ap.add_argument("--drop-range", nargs=2, action="append", default=[], metavar=("FIRST", "LAST"))
    ap.add_argument("--block-out", required=True)
    ap.add_argument("--replace-with", required=True)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    def hit(line, text):
        return line.startswith(text) if a.match == "prefix" else line == text

    def unique(lines, text, what, lo=0):
        found = [i for i in range(lo, len(lines)) if hit(lines[i], text)]
        if len(found) != 1:
            die(f"{what} {text!r} matched {len(found)} lines (need exactly 1)")
        return found[0]

    path = pathlib.Path(a.file)
    lines = path.read_text().split("\n")
    s = unique(lines, a.start, "--start") + a.start_offset
    e = unique(lines, a.end_after, "--end-after")
    if not 0 <= s < e:
        die(f"--start (line {s + 1}) must precede --end-after (line {e + 1})")
    block = lines[s:e]

    spans = []
    for d in a.drop:
        i = unique(block, d, "--drop")
        spans.append((i, i))
    for first, last in a.drop_range:
        i = unique(block, first, "--drop-range FIRST")
        j = unique(block, last, "--drop-range LAST", lo=i)
        spans.append((i, j))
    spans.sort()
    dropped_idx = set()
    for i, j in spans:
        for k in range(i, j + 1):
            if k in dropped_idx:
                die("overlapping --drop / --drop-range")
            dropped_idx.add(k)
    kept = [block[k] for k in sorted(dropped_idx)]
    moved = [l for k, l in enumerate(block) if k not in dropped_idx]

    repl = pathlib.Path(a.replace_with).read_text().split("\n")
    if repl and repl[-1] == "":
        repl = repl[:-1]
    markers = [i for i, l in enumerate(repl) if l.strip() == MARKER]
    if kept and len(markers) != 1:
        die(f"{len(kept)} dropped line(s) need exactly one {MARKER} line in --replace-with")
    if not kept and markers:
        die(f"{MARKER} in --replace-with but nothing was dropped")
    if markers:
        m = markers[0]
        repl = repl[:m] + kept + repl[m + 1:]

    print(f"carve-block: {a.file} lines {s + 1}-{e} ({e - s} lines), "
          f"moved {len(moved)}, kept {len(kept)}")
    if a.dry_run:
        return
    pathlib.Path(a.block_out).write_text("\n".join(moved) + "\n")
    path.write_text("\n".join(lines[:s] + repl + lines[e:]))


if __name__ == "__main__":
    main()
