#!/usr/bin/env bash
# delete-modules.sh
#
# Purpose: remove a batch of dead C modules from the tree in one deterministic,
# idempotent run. It works in two passes, and the order matters.
#
# PASS 1 — SCAN, mutates nothing:
#   For every `src/**/*.c` in the list, collect its exported symbols and look
#   for any surviving production `.c` under src/ or apps/ (outside the batch)
#   that still names one. Such a reference contradicts the premise that the
#   module is dead. If PASS 1 finds any, the script prints every offending
#   file:line and EXITS 1 WITHOUT TOUCHING THE TREE. `--force` downgrades this
#   to a warning and proceeds; use it only when each reference has been read
#   and shown to be a comment or otherwise inert.
#
# PASS 2 — MUTATE, only reached when PASS 1 is clean (or --force):
#   1. `git rm` the .c file
#   2. strip the file's line from every CMake source list (CMakeLists.txt and
#      any *.cmake) — one path per line is the project's convention
#   3. resolve the module's own header (the `#include` in the .c whose path
#      ends in `<basename>.h`) and `git rm` it IFF no surviving tracked file
#      still includes it; otherwise keep it and say who still includes it
#
# It never edits a test file and never edits a .c outside the list. Test files
# referencing a deleted symbol, and CMake lines that pack several paths onto
# one line, are REPORTED — in the per-module output and again in the summary —
# for a human to resolve; the script does not rewrite either.
#
# Re-running after the tests have been handled is expected: the header prune in
# step 3 only becomes decidable once the test files that included the header
# are gone. Re-running on an already-processed tree is a no-op.
#
# Exported symbols come from the warm build's object files when available
# (`build/CMakeFiles/human_core.dir/<path>.o`), which is exact; otherwise they
# are parsed out of the .c, which is approximate but adequate for the reports.
#
# Usage:
#   scripts/dev/delete-modules.sh <list-file> [--dry-run] [--force]
#
#   <list-file>  one `src/...` path per line; blank lines and #comments ignored
#   --dry-run    print every action, change nothing
#   --force      proceed even when PASS 1 finds a production reference
#
# Exit status:
#   0  batch processed (or --dry-run completed)
#   1  PASS 1 found a production reference; nothing was changed
#   2  the list or the repo root is unusable

set -euo pipefail

usage() {
    echo "usage: $0 <list-file> [--dry-run] [--force]" >&2
    exit 2
}

LIST=""
DRY_RUN=0
FORCE=0
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        --force) FORCE=1 ;;
        -h | --help) usage ;;
        -*) echo "unknown option: $arg" >&2; usage ;;
        *) [ -n "$LIST" ] && usage; LIST="$arg" ;;
    esac
done
[ -n "$LIST" ] || usage
[ -r "$LIST" ] || { echo "cannot read list: $LIST" >&2; exit 2; }

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Every .c in the batch: used to exclude the batch's own members when asking
# "does anything still include this header" / "does any production .c still
# name this symbol".
grep -vE '^\s*(#|$)' "$LIST" | sed 's/[[:space:]]*$//' > "$WORK/batch.txt"

say() { printf '%s\n' "$*"; }
run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        say "    would run: $*"
    else
        "$@"
    fi
}

# Is this path one of the batch members (i.e. about to disappear)?
in_batch() { grep -qxF "$1" "$WORK/batch.txt"; }

# Print the .c's source text: from the working tree if it is still there,
# else from HEAD, else from the newest commit that still had it. Re-running
# this script after the .c has been removed must still resolve the header,
# so header pruning stays decidable on the second pass (the first pass
# usually keeps headers that test files, since deleted, still included) and
# the summary stays honest after the deletion is committed.
module_text() {
    local c="$1" rev
    if [ -f "$c" ]; then cat "$c"; return 0; fi
    if git cat-file -e "HEAD:$c" 2>/dev/null; then git show "HEAD:$c"; return 0; fi
    rev="$(git rev-list -1 HEAD -- "$c" 2>/dev/null || true)"
    [ -n "$rev" ] && git show "$rev^:$c" 2>/dev/null || true
}

# Resolve the module's own header from its #include lines.
# Prints the repo-relative path, or nothing.
resolve_header() {
    local c="$1" base inc cand
    base="$(basename "$c" .c)"
    inc="$(module_text "$c" | grep -o '#include "[^"]*"' 2>/dev/null \
        | sed 's/#include "//; s/"$//' \
        | grep -E "(^|/)${base}\.h$" | head -1 || true)"
    [ -n "$inc" ] || return 0
    for cand in "include/$inc" "$(dirname "$c")/$inc" "$(dirname "$c")/$(basename "$inc")"; do
        if [ -f "$cand" ]; then printf '%s\n' "$cand"; return 0; fi
    done
}

# Exported (non-static, external) symbols of the module.
exported_symbols() {
    local c="$1" obj="build/CMakeFiles/human_core.dir/$1.o"
    if [ -f "$obj" ]; then
        nm "$obj" 2>/dev/null | awk '$2 == "T" { print $3 }' | sed 's/^_//' | sort -u
    else
        # Fallback: column-0 definitions that are not `static`.
        module_text "$c" \
            | grep -oE '^[A-Za-z_][A-Za-z0-9_ *]*\bhu_[a-z0-9_]+[[:space:]]*\(' 2>/dev/null \
            | grep -oE 'hu_[a-z0-9_]+' | sort -u
    fi
}

n_c_removed=0
n_c_absent=0
n_cmake_lines=0
n_hdr_removed=0
n_hdr_kept=0
: > "$WORK/tests.txt"
: > "$WORK/kept-headers.txt"
: > "$WORK/prod-refs.txt"
: > "$WORK/composite.txt"

# ── PASS 1: scan only. Nothing below this block may modify the tree. ─────────
say "--- PASS 1: scanning $(wc -l < "$WORK/batch.txt" | tr -d ' ') modules for production references"
while read -r c; do
    [ -n "$c" ] || continue
    exported_symbols "$c" > "$WORK/syms-$(echo "$c" | tr / _).txt"
    while read -r s; do
        [ -n "$s" ] || continue
        # A surviving production .c that still names this symbol contradicts
        # the premise that the module is dead. Record file:line; PASS 2 is
        # refused unless every hit has been read and --force passed.
        while read -r hit; do
            [ -n "$hit" ] || continue
            p="${hit%%:*}"
            [ "$p" = "$c" ] && continue
            in_batch "$p" && continue
            case "$p" in *.c) ;; *) continue ;; esac
            echo "$s	$hit" >> "$WORK/prod-refs.txt"
        done < <(grep -rnw "$s" src apps --include='*.c' 2>/dev/null || true)

        while read -r t; do
            [ -n "$t" ] || continue
            echo "$t	$s" >> "$WORK/tests.txt"
        done < <(grep -rlw "$s" tests 2>/dev/null || true)
    done < "$WORK/syms-$(echo "$c" | tr / _).txt"
done < "$WORK/batch.txt"

if [ -s "$WORK/prod-refs.txt" ]; then
    say ""
    say "!! PASS 1 FOUND PRODUCTION REFERENCES to supposedly-dead symbols:"
    sort -u "$WORK/prod-refs.txt" | sed 's/^/    /'
    if [ "$FORCE" -eq 0 ]; then
        say ""
        say "Nothing was removed. Read each hit above: if every one is a comment or"
        say "otherwise inert, re-run with --force. If any is a live call, the module"
        say "is NOT dead — take it out of the list."
        exit 1
    fi
    say ""
    say "--force given: proceeding despite the references above."
else
    say "--- PASS 1 clean: no production .c names any symbol in this batch"
fi

# ── PASS 2: mutate. ─────────────────────────────────────────────────────────
say ""
say "--- PASS 2: applying"
while read -r c; do
    [ -n "$c" ] || continue
    say "=== $c"

    if [ ! -f "$c" ]; then
        say "    .c already gone (idempotent skip)"
        n_c_absent=$((n_c_absent + 1))
    fi

    nsyms=$(wc -l < "$WORK/syms-$(echo "$c" | tr / _).txt" | tr -d ' ')
    say "    exported symbols: $nsyms"

    # --- header ---
    hdr="$(resolve_header "$c")"
    if [ -n "$hdr" ]; then
        rel="${hdr#include/}"
        # Who still includes it, ignoring the batch and the header itself?
        # Match the FULL path ("human/security/replay.h") and every relative
        # suffix of it ("security/replay.h", "replay.h") — umbrella headers
        # like include/human/human.h include their siblings relative to
        # themselves, and a full-path-only grep silently misses those.
        : > "$WORK/includers.txt"
        : > "$WORK/hdr-pats.txt"
        pat="$rel"
        while [ -n "$pat" ]; do
            printf '#include "%s"\n' "$pat" >> "$WORK/hdr-pats.txt"
            case "$pat" in */*) pat="${pat#*/}" ;; *) pat="" ;; esac
        done
        while read -r f; do
            [ -n "$f" ] || continue
            [ "$f" = "$hdr" ] && continue
            in_batch "$f" && continue
            echo "$f" >> "$WORK/includers.txt"
        done < <(grep -rlFf "$WORK/hdr-pats.txt" src apps include tests 2>/dev/null || true)

        if [ -s "$WORK/includers.txt" ]; then
            say "    header KEPT: $hdr (still included by $(wc -l < "$WORK/includers.txt" | tr -d ' ') file(s))"
            sed 's/^/        includer: /' "$WORK/includers.txt"
            echo "$hdr" >> "$WORK/kept-headers.txt"
            n_hdr_kept=$((n_hdr_kept + 1))
        else
            say "    header ORPHANED, removing: $hdr"
            run git rm -q "$hdr"
            n_hdr_removed=$((n_hdr_removed + 1))
        fi
    else
        say "    header: none of its own (declared in a shared header)"
    fi

    # --- CMake source lists ---
    while read -r cm; do
        [ -n "$cm" ] || continue
        if grep -qE "^[[:space:]]*${c//\//\\/}[[:space:]]*$" "$cm"; then
            n=$(grep -cE "^[[:space:]]*${c//\//\\/}[[:space:]]*$" "$cm")
            say "    cmake: dropping $n line(s) from $cm"
            if [ "$DRY_RUN" -eq 1 ]; then
                say "    would edit: $cm"
            else
                grep -vE "^[[:space:]]*${c//\//\\/}[[:space:]]*$" "$cm" > "$WORK/cm.tmp"
                cat "$WORK/cm.tmp" > "$cm"
            fi
            n_cmake_lines=$((n_cmake_lines + n))
        fi
        # A path embedded in a longer line (list(APPEND ...) etc.) is NOT
        # something this script will rewrite — flag it for a human.
        if grep -nF "$c" "$cm" | grep -qvE "^[0-9]+:[[:space:]]*${c//\//\\/}[[:space:]]*$"; then
            say "    !! cmake: $c appears inside a composite line in $cm — handle by hand:"
            grep -nF "$c" "$cm" \
                | grep -vE "^[0-9]+:[[:space:]]*${c//\//\\/}[[:space:]]*$" \
                | sed 's/^/        /'
            grep -nF "$c" "$cm" \
                | grep -vE "^[0-9]+:[[:space:]]*${c//\//\\/}[[:space:]]*$" \
                | while read -r hit; do echo "$cm:${hit%%:*}  $c"; done >> "$WORK/composite.txt"
        fi
    done < <(git ls-files '*CMakeLists.txt' '*.cmake')

    # --- the .c itself ---
    if [ -f "$c" ]; then
        run git rm -q "$c"
        n_c_removed=$((n_c_removed + 1))
    fi
done < "$WORK/batch.txt"

say ""
say "================ SUMMARY ================"
say "modules in batch      : $(wc -l < "$WORK/batch.txt" | tr -d ' ')"
say ".c removed            : $n_c_removed"
say ".c already absent     : $n_c_absent"
say "headers removed       : $n_hdr_removed"
say "headers kept (shared) : $n_hdr_kept"
say "cmake lines dropped   : $n_cmake_lines"
if [ -s "$WORK/kept-headers.txt" ]; then
    say ""
    say "Headers kept — strip the deleted modules' prototypes by hand:"
    sort -u "$WORK/kept-headers.txt" | sed 's/^/    /'
fi
if [ -s "$WORK/composite.txt" ]; then
    say ""
    say "!! CMake lines this script REFUSED to rewrite ($(sort -u "$WORK/composite.txt" | wc -l | tr -d ' ') — several paths on one line; edit by hand):"
    sort -u "$WORK/composite.txt" | sed 's/^/    /'
fi
if [ -s "$WORK/prod-refs.txt" ]; then
    say ""
    say "!! PRODUCTION references to supposedly-dead symbols (--force was used to get here):"
    sort -u "$WORK/prod-refs.txt" | sed 's/^/    /'
fi
if [ -s "$WORK/tests.txt" ]; then
    say ""
    say "Test files referencing deleted symbols ($(cut -f1 "$WORK/tests.txt" | sort -u | wc -l | tr -d ' ') files):"
    say "  delete the file outright if EVERY symbol it exercises is listed here;"
    say "  otherwise drop just the dead test functions and their registration."
    cut -f1 "$WORK/tests.txt" | sort | uniq -c | sort -rn | sed 's/^/    /'
fi
say "========================================="
