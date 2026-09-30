#!/usr/bin/env bash
# scripts/prune-includes.sh — drop every #include a carved C file does not need.
#
# hu_agent_turn carve (docs/superpowers/plans/2026-09-30-agent-turn-carve-phase1.md):
# a stage file starts with agent_turn.c's whole include block. This script
# removes each #include that BOTH compile commands recorded for the file in
# build/compile_commands.json (the daemon library and the HU_IS_TEST library)
# can live without. "Can live without" does NOT rely on the symbol table
# alone — a dropped header can flip an #ifdef without changing a single
# exported symbol. So for each candidate line the script compiles the file
# with and without it (adding -g0 to both, so DWARF differences never cause a
# false mismatch) and requires ALL of: the file still compiles with -Werror,
# each object's symbol table (type + name, from nm) is byte-identical, AND
# each object's __TEXT,__text / __DATA,__data / __TEXT,__const / __DATA,__const
# section bytes are byte-identical. The include is removed only when every
# recorded compile command agrees on all three. Afterwards, "#if…" lines
# directly followed by "#endif" (guards whose includes were all dropped) are
# removed.
#
# An include needed only by a configuration that is not in build/ (no-sqlite,
# minimal) can be dropped by this; scripts/verify-carve-stage.sh builds those
# configurations and will fail — re-add that include by hand.
#
# Usage: bash scripts/prune-includes.sh src/agent/turn/turn_retrieve.c
set -euo pipefail
[ $# -eq 1 ] || { echo "usage: $0 <file.c>" >&2; exit 2; }
root="$(git rev-parse --show-toplevel)"
file="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
db="${HU_BUILD_DIR:-$root/build}/compile_commands.json"
[ -f "$db" ] || { echo "prune-includes: no $db — run: cmake --preset dev" >&2; exit 2; }
work="$(mktemp -d "${TMPDIR:-/tmp}/prune-includes.XXXXXX")"
probe="${file%.c}.prune_probe.c"
trap 'rm -rf "$work" "$probe"' EXIT

python3 - "$db" "$file" > "$work/cmds" <<'PY'
import json, shlex, sys
db, target = sys.argv[1], sys.argv[2]
for e in json.load(open(db)):
    if e["file"] != target:
        continue
    args = e.get("arguments") or shlex.split(e["command"])
    out, skip = [], False
    for arg in args:
        if skip:
            skip = False
            continue
        if arg in ("-o", "-c"):
            skip = True
            continue
        if arg == target:
            continue
        out.append(arg)
    print(e["directory"] + "\t" + shlex.join(out))
PY
n_cmds=$(grep -c . "$work/cmds" || true)
[ "$n_cmds" -ge 1 ] || {
    echo "prune-includes: no compile command for $file — register it in CMakeLists.txt, rebuild" >&2
    exit 2
}

# section_bytes OBJ: print the hex bytes (address column stripped, so a
# section that merely starts at a different offset doesn't look different)
# of every section this check cares about, or nothing if the section is
# absent (a legitimately-dropped __DATA,__const is not a mismatch).
section_bytes() {
    local obj="$1" seg sect
    for pair in "__TEXT __text" "__DATA __data" "__TEXT __const" "__DATA __const"; do
        # shellcheck disable=SC2086
        set -- $pair
        seg="$1"; sect="$2"
        otool -s "$seg" "$sect" "$obj" 2>/dev/null | tail -n +3 | awk '{$1=""; print}'
    done
}

# symbols SRC TAG: compile SRC with every recorded command (each with -g0
# appended, so debug info can never be the source of a mismatch) and store
# each object's sorted symbol table in $work/TAG.<i> and its section bytes
# (see section_bytes) in $work/TAG.<i>.sec. Non-zero if a compile fails.
symbols() {
    local src="$1" tag="$2" i=0 dir cmd
    while IFS=$'\t' read -r dir cmd; do
        i=$((i + 1))
        if ! (cd "$dir" && eval "$cmd -g0 -c \"\$src\" -o \"\$work/obj.$i.o\"") > "$work/log" 2>&1; then
            return 1
        fi
        nm "$work/obj.$i.o" | awk '{print $(NF-1), $NF}' | sort > "$work/$tag.$i"
        section_bytes "$work/obj.$i.o" > "$work/$tag.$i.sec"
    done < "$work/cmds"
}

# The baseline is compiled from the probe PATH too: under ASan the object's
# __TEXT,__const embeds the source file name, so a baseline built from
# "$file" differs from every probe built from "$probe" and nothing is ever
# dropped (measured 2026-09-30 on turn_retrieve.c: 0 of 177 removed).
cp "$file" "$probe"
if ! symbols "$probe" base; then
    cat "$work/log" >&2
    echo "prune-includes: $file does not compile before pruning" >&2
    exit 1
fi

removed=0
total=$(grep -c '^#include' "$file" || true)
done_n=0
for ln in $(grep -n '^#include' "$file" | cut -d: -f1 | sort -rn); do
    done_n=$((done_n + 1))
    line="$(sed -n "${ln}p" "$file")"
    case "$line" in *'"human/agent/turn.h"'*) continue ;; esac
    sed "${ln}d" "$file" > "$probe"
    symbols "$probe" probe || continue
    same=1
    for i in $(seq 1 "$n_cmds"); do
        cmp -s "$work/base.$i" "$work/probe.$i" || same=0
        cmp -s "$work/base.$i.sec" "$work/probe.$i.sec" || same=0
    done
    if [ "$same" -eq 1 ]; then
        cp "$probe" "$file"
        removed=$((removed + 1))
        echo "[$done_n/$total] dropped: $line"
    fi
done

python3 - "$file" <<'PY'
import re, sys
p = sys.argv[1]
lines = open(p).read().split("\n")
changed = True
while changed:
    changed = False
    for i in range(len(lines) - 1):
        if re.match(r"#\s*if", lines[i]) and re.match(r"#\s*endif", lines[i + 1]):
            del lines[i:i + 2]
            changed = True
            break
open(p, "w").write("\n".join(lines))
PY
echo "prune-includes: removed $removed include(s); $(grep -c '^#include' "$file") kept in $file"
