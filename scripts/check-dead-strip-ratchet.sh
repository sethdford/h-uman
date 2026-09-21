#!/usr/bin/env bash
# check-dead-strip-ratchet.sh
#
# Dead-code ratchet whose oracle is the LINKER, not a heuristic
# (docs/plans/2026-09-20-dead-code-plan.md §6). Relinks `human` with
# -dead_strip and a link map, then counts two things that may only shrink:
#
#   A  never-loaded archive members — objects in libhuman_core.a none of whose
#      global symbols the linker ever saw, live or dead-stripped. A whole
#      translation unit nobody links.
#   B  unreferenced dead symbols — `_hu_` symbols the linker dead-stripped out
#      of libhuman_core.a members that no human_tests object references either.
#      Dead in the product AND unpinned by a test.
#
# USAGE
#   bash scripts/check-dead-strip-ratchet.sh
#   HU_BUILD_DIR=build2 bash scripts/check-dead-strip-ratchet.sh
#
# EXIT CODES
#   0  A and B are at or below their baselines — or the gate could not measure
#      (no build dir, non-Darwin, stale build dir) and said so. An unmeasurable
#      gate must never block a push (.claude/rules/no-number-without-a-measurement.md);
#      it prints `RATCHET_SKIP: <reason>` so scripts/ratchet-debt-report.sh can
#      tell "not measured here" from "measurement broke".
#   1  A or B grew past its baseline. The offending names are printed.
#
# THE BASELINES ARE CONFIGURATION-SPECIFIC. They were measured against the dev
# preset in `build/` (ASan, the full feature set). A differently-configured tree
# compiles a different set of translation units, so pointing HU_BUILD_DIR at
# e.g. build-check measures a DIFFERENT universe and its numbers are not
# comparable to these constants. See .claude/rules/dead-strip-ratchet.md.
set -euo pipefail

# Auto-lock any gain so it can never be spent again (scripts/ratchet-config.tsv).
# Sourced defensively, for the same reason check-clone-ratchet.sh does: the gate
# must keep BLOCKING growth in a tree where the helper is absent, so a missing
# helper degrades to "no auto-lock" rather than "commit refused".
_hu_root="$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
if [ -r "$_hu_root/scripts/lib/ratchet.sh" ]; then
    . "$_hu_root/scripts/lib/ratchet.sh"
else
    ratchet_autolock() { :; }
fi

# Measured 2026-09-21 on 4d376689b, against build/ (dev preset, ASan), after
# Tasks 6-9 deleted the 105 unreferenced dead functions and the 37 abandoned
# modules. 1,024 archive members in human_core.dir/link.txt; of those 56 export
# no global T symbol at all (embedded data blobs data_prompts_*_txt.c.o and the
# outbound/{strip,shape,echo,persona,moderation}.c.o stage tables, which export
# only D/S) and are excluded from A by construction — a member with no code in
# it is not a never-loaded module.
NEVER_LOADED_BASELINE=50
# Composition at the baseline: 40 whole function symbols plus 59 function-local
# statics (`_hu_fn.CONSTANT`, `_hu_fn.sql`), which the linker emits as separate
# symbols of the function that owns them. Both are counted, per the plan's
# definition; the statics move with their function, so they are correlated
# rather than independent noise. The FAIL output prints every name.
DEAD_UNREF_BASELINE=99

cd "$_hu_root"

BUILD_DIR="${HU_BUILD_DIR:-build}"
HUMAN_LINK="$BUILD_DIR/CMakeFiles/human.dir/link.txt"
CORE_LINK="$BUILD_DIR/CMakeFiles/human_core.dir/link.txt"

skip() {
    echo "RATCHET_SKIP: $1"
    echo "dead-strip ratchet did not run: $1" >&2
    exit 0
}

# The map parser below is macOS ld's (`# Object files:` / `# Symbols:` /
# `# Dead Stripped Symbols:`). GNU ld's -Map is a different format; until that
# is implemented, say so rather than measuring something else.
[ "$(uname -s)" = "Darwin" ] || skip "unsupported platform ($(uname -s)); macOS ld link maps only"

[ -f "$HUMAN_LINK" ] || skip "no $HUMAN_LINK (configure and build first)"
[ -f "$CORE_LINK" ]  || skip "no $CORE_LINK (configure and build first)"

# A build dir older than the sources describes a different tree, so its counts
# cannot fail THIS push. Report them, flagged, and exit 0.
STALE=0
if [ -f "$BUILD_DIR/libhuman_core.a" ]; then
    if [ -n "$(find src include -name '*.[ch]' -newer "$BUILD_DIR/libhuman_core.a" -print -quit 2>/dev/null)" ]; then
        STALE=1
    fi
else
    skip "no $BUILD_DIR/libhuman_core.a (build the human + human_tests targets first)"
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/hu-dead-strip.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

# --- 1. relink with -dead_strip + a link map ------------------------------
# link.txt is written to be run from inside the build dir and links to `human`;
# redirect the output so this gate never clobbers the real binary.
link_cmd=$(sed "s| -o human | -o $TMP/human_ds |" "$HUMAN_LINK")
case "$link_cmd" in
    *"$TMP/human_ds"*) ;;
    *) skip "could not redirect the link in $HUMAN_LINK (no ' -o human ' to replace)" ;;
esac
if ! ( cd "$BUILD_DIR" && sh -c "$link_cmd -Wl,-dead_strip -Wl,-map,$TMP/human.map" ) >"$TMP/link.log" 2>&1; then
    sed 's/^/  /' "$TMP/link.log" >&2
    skip "the -dead_strip relink failed (see the log above)"
fi
[ -s "$TMP/human.map" ] || skip "the relink produced no link map"

# --- 2. parse the map ----------------------------------------------------
# Emits one TSV row per entry: O = object-file line, L = live symbol,
# D = dead-stripped symbol. Field 2 is the `[ N]` object index, field 3 the
# path (O) or symbol name (L/D).
awk '
  /^# Object files:/           { sec="obj";  next }
  /^# Sections:/               { sec="";     next }
  /^# Symbols:/                { sec="live"; next }
  /^# Dead Stripped Symbols:/  { sec="dead"; next }
  /^#/ { next }
  sec=="obj" {
      if (match($0, /^\[ *[0-9]+\] /)) {
          idx = substr($0, 2, RLENGTH - 3); gsub(/ /, "", idx)
          print "O\t" idx "\t" substr($0, RLENGTH + 1)
      }
      next
  }
  sec=="live" || sec=="dead" {
      i = index($0, "]")
      if (i) {
          split(substr($0, 1, i), a, "[")
          idx = a[2]; gsub(/[] ]/, "", idx)
          print (sec=="live" ? "L" : "D") "\t" idx "\t" substr($0, i + 2)
      }
      next
  }
' "$TMP/human.map" > "$TMP/map.tsv"

awk -F'\t' '$1=="L" || $1=="D" { print $3 }' "$TMP/map.tsv" | sort -u > "$TMP/map_syms.txt"

# --- 3. count A: never-loaded archive members ----------------------------
# The object universe is link.txt, not the map's own `# Object files:` list:
# that list names archive members by BASENAME, and this tree has ~90 duplicated
# basenames across layers (two dispatch.c, two client.c, ...), so basename
# attribution is wrong by construction. Attribute by symbol name instead.
# link.txt names its members relative to the build dir (it is run from there);
# this gate runs from the repo root, so re-root them or every nm lookup misses
# and A collapses to 0.
tr ' ' '\n' < "$CORE_LINK" | grep '\.o$' | sed "s|^|$BUILD_DIR/|" | sort -u > "$TMP/members.txt"
xargs nm -g --defined-only < "$TMP/members.txt" > "$TMP/nm_members.txt" 2>/dev/null || true
# nm reads every member or this gate measures nothing: an unreadable path makes
# A collapse silently to 0, which reads as a clean sweep. Refuse instead.
n_members=$(wc -l < "$TMP/members.txt" | tr -d ' ')
n_read=$(grep -c ':$' "$TMP/nm_members.txt" || true)
[ "$n_read" = "$n_members" ] || \
    skip "nm read $n_read of $n_members archive members (stale or moved objects in $BUILD_DIR)"

# Common symbols (nm type C) are EXCLUDED from the liveness test. Under ASan
# every object defines ____asan_globals_registered as a common symbol, and the
# linker coalesces them into one map entry, so testing that name by set
# membership would mark all 1,024 members live and collapse A to 0. Only
# T/D/S/R-style defined symbols identify their own object.
#
# nm prints a `path:` header before each file's symbols when handed more than
# one file; those headers are the per-object boundary below.
awk -v symfile="$TMP/map_syms.txt" '
  BEGIN { while ((getline s < symfile) > 0) inmap[s] = 1 }
  function emit() { if (hasT && !live) print cur }
  /^$/ { next }
  /:$/ && $0 !~ /^[0-9a-f]+ / { if (cur != "") emit(); cur = substr($0, 1, length($0) - 1); hasT = 0; live = 0; next }
  {
      if (NF < 3) next
      if ($2 == "C") next
      if ($2 == "T") hasT = 1
      if ($3 in inmap) live = 1
  }
  END { if (cur != "") emit() }
' "$TMP/nm_members.txt" > "$TMP/never_loaded.txt"
A=$(wc -l < "$TMP/never_loaded.txt" | tr -d ' ')

# --- 4. count B: dead _hu_ symbols no test references --------------------
awk -F'\t' '$1=="O" && $3 ~ /libhuman_core\.a\(/ { print $2 }' "$TMP/map.tsv" | sort -u > "$TMP/core_idx.txt"
awk -F'\t' -v idxfile="$TMP/core_idx.txt" '
  BEGIN { while ((getline i < idxfile) > 0) core[i] = 1 }
  $1=="D" && ($2 in core) && $3 ~ /^_hu_/ { print $3 }
' "$TMP/map.tsv" | sort -u > "$TMP/dead_hu.txt"

# The test-reference set is the slow half (~2,000 objects), and it only changes
# when a test object is recompiled — so cache it on the newest test .o mtime.
TESTREFS=""
find "$BUILD_DIR/CMakeFiles/human_tests.dir" "$BUILD_DIR/CMakeFiles/human_core_test.dir" \
     -name '*.o' > "$TMP/testobjs.txt" 2>/dev/null || true
if [ -s "$TMP/testobjs.txt" ]; then
    # max via awk, not `sort -rn | head -1`: under `set -o pipefail` head's early
    # exit SIGPIPEs sort as soon as its output outgrows the pipe buffer, and the
    # non-zero status would kill the script under `set -e`. Silent today at
    # ~22 KB, a time bomb as the suite grows.
    newest=$(xargs stat -f '%m' < "$TMP/testobjs.txt" 2>/dev/null | awk '$1 > m { m = $1 } END { print m + 0 }')
    nobj=$(wc -l < "$TMP/testobjs.txt" | tr -d ' ')
    dirkey=$(printf '%s' "$(pwd)/$BUILD_DIR" | cksum | tr -d ' ' | cut -c1-16)
    cache_dir="${TMPDIR:-/tmp}/hu-dead-strip-cache"
    mkdir -p "$cache_dir"
    TESTREFS="$cache_dir/testrefs-$dirkey-${newest:-0}-$nobj.txt"
    if [ ! -s "$TESTREFS" ]; then
        xargs nm -u < "$TMP/testobjs.txt" 2>/dev/null \
            | awk 'NF==1 { print $1 }' | sort -u > "$TESTREFS.$$" || true
        mv -f "$TESTREFS.$$" "$TESTREFS"
    fi
fi
if [ -s "${TESTREFS:-/nonexistent}" ]; then
    comm -23 "$TMP/dead_hu.txt" "$TESTREFS" > "$TMP/dead_unref.txt"
else
    # No test objects built: every dead _hu_ symbol is unreferenced BY THIS
    # MEASUREMENT, which is not the same claim. Refuse rather than inflate B.
    skip "no built test objects under $BUILD_DIR/CMakeFiles/human_{tests,core_test}.dir"
fi
B=$(wc -l < "$TMP/dead_unref.txt" | tr -d ' ')

# --- 5. report + ratchet -------------------------------------------------
echo "Relinked $BUILD_DIR/human with -dead_strip ($(wc -l < "$TMP/members.txt" | tr -d ' ') archive members)..."
echo "A = never-loaded archive members: $A (ceiling $NEVER_LOADED_BASELINE)"
echo "B = unreferenced dead symbols: $B (ceiling $DEAD_UNREF_BASELINE)"

if [ "$STALE" = "1" ]; then
    echo "RATCHET_SKIP: $BUILD_DIR is older than src/ — counts describe an earlier tree"
    echo "NOTE: $BUILD_DIR predates the current sources, so these counts are advisory." >&2
    echo "      Rebuild ('cmake --build $BUILD_DIR --target human human_tests') to enforce." >&2
    exit 0
fi

ratchet_autolock NEVER_LOADED_BASELINE "$A" "scripts/check-dead-strip-ratchet.sh"
ratchet_autolock DEAD_UNREF_BASELINE   "$B" "scripts/check-dead-strip-ratchet.sh"

fail=0
if [ "$A" -gt "$NEVER_LOADED_BASELINE" ]; then
    echo "FAIL: never-loaded archive members grew. Baseline: $NEVER_LOADED_BASELINE, current: $A" >&2
    echo "      Every object below is compiled, archived, and never linked into human:" >&2
    sed 's/^/  /' "$TMP/never_loaded.txt" >&2
    echo "      Delete it, or wire it — see .claude/rules/dead-strip-ratchet.md." >&2
    fail=1
elif [ "$A" -lt "$NEVER_LOADED_BASELINE" ]; then
    [ "${HU_RATCHET_LOCKED:-0}" = 1 ] || \
    echo "NOTE: never-loaded members dropped to $A — lower NEVER_LOADED_BASELINE to lock the gain." >&2
fi

if [ "$B" -gt "$DEAD_UNREF_BASELINE" ]; then
    echo "FAIL: unreferenced dead symbols grew. Baseline: $DEAD_UNREF_BASELINE, current: $B" >&2
    echo "      Each symbol below is dead-stripped from human and referenced by no test:" >&2
    sed 's/^/  /' "$TMP/dead_unref.txt" >&2
    echo "      Delete it, or give it a caller — see .claude/rules/dead-strip-ratchet.md." >&2
    fail=1
elif [ "$B" -lt "$DEAD_UNREF_BASELINE" ]; then
    [ "${HU_RATCHET_LOCKED:-0}" = 1 ] || \
    echo "NOTE: unreferenced dead symbols dropped to $B — lower DEAD_UNREF_BASELINE to lock the gain." >&2
fi

exit $fail
