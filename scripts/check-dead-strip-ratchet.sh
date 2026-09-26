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
#   bash scripts/check-dead-strip-ratchet.sh                  # ad-hoc / pre-commit
#   HU_DEAD_STRIP_STRICT=1 bash scripts/check-dead-strip-ratchet.sh   # pre-push
#   HU_BUILD_DIR=build2 bash scripts/check-dead-strip-ratchet.sh
#
# TWO CALLERS, TWO JOBS
#   .githooks/pre-commit  runs it plain, so ratchet_autolock can rewrite and
#                         STAGE a lowered constant — that only happens from the
#                         pre-commit hook (HU_RATCHET_FROM_HOOK=1), so this is
#                         the only place a gain can ever be locked.
#   .githooks/pre-push    rebuilds build/ first, then runs it with
#                         HU_DEAD_STRIP_STRICT=1. That is the ENFORCEMENT point.
#
#   HU_DEAD_STRIP_STRICT=1 disables the stale-build demotion below. Without it
#   (ad-hoc runs, and pre-commit) a build dir older than src/ reports its counts
#   as advisory and exits 0, because they describe a different tree.
#
# EXIT CODES
#   0  A and B are at or below their baselines — or the gate could not measure
#      (no build dir, build dir not configured as the dev preset, non-Darwin,
#      stale build dir outside strict mode) and said
#      so. An unmeasurable gate must never block a commit or a push
#      (.claude/rules/no-number-without-a-measurement.md); it prints
#      `RATCHET_SKIP: <reason>` so scripts/ratchet-debt-report.sh can tell
#      "not measured here" from "measurement broke".
#   1  A or B grew past its baseline. The offending names are printed.
#
# THE BASELINES ARE CONFIGURATION-SPECIFIC. They were measured against the dev
# preset in `build/` (ASan, the full feature set). A differently-configured tree
# compiles a different set of translation units, so pointing HU_BUILD_DIR at
# e.g. build-check measures a DIFFERENT universe and its numbers are not
# comparable to these constants. The gate checks this rather than assuming it:
# any build dir whose CMakeCache.txt disagrees with the dev preset is skipped
# (RATCHET_SKIP), strict mode or not. See .claude/rules/dead-strip-ratchet.md.
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
#
# Re-measured 2026-09-21, same day, after Task 11 (docs/plans/2026-09-20-
# dead-code-plan.md) turned HU_ENABLE_ALL_CHANNELS off in the dev preset (991
# archive members, down from 1,024 — the ~16 channels no build ever configures
# stopped compiling). This is a manual lock, not an auto-lock: the pre-commit
# hook's ratchet_autolock only fires when a staged src/**/*.c or *.h changes,
# and Task 11 touched only CMakeLists.txt/CMakePresets.json, so the mechanism
# never ran. The commit that changed the presets also fixed two never-loaded
# members the config change exposed (meta_common.c's stray "OR
# HU_ENABLE_WHATSAPP", and src/voice/webrtc*.c sitting unconditionally in
# HU_CORE_SOURCES); this baseline reflects the fixed tree, not the raw
# preset flip.
#
# Re-measured 2026-09-21 after Task 12 (daba0dfb2) moved 12 test/eval/SDK
# modules out of human_core into the new human_devlib archive: 983 members,
# down from 994, and A 47 -> 36 — the 11 moved modules that this preset
# compiled into human_core (the 12th, src/channels/dispatch.c, was
# ALL_CHANNELS-gated and so absent from the dev preset's core archive).
# Nothing was deleted; libhuman_core.a now holds daemon code only, which is
# what makes A mean "dead" rather than "library the daemon doesn't use".
# Manual lock again (2026-09-21, Task 15): deleting the unwired
# cot_audit_validator wrapper (zero production callers; the live CoT audit
# is a direct hu_cot_audit() call in agent_turn.c) dropped a freshly-built
# measurement from 36 to 33. The auto-lock again did not fire — the shared
# core.hooksPath resolves to the main checkout's .githooks/pre-commit, which
# predates the auto-lock block added to this worktree's copy — so this
# constant is set by hand to the number the gate printed on a freshly built
# tree, same as the prior manual lock below.
# Manual lock again (2026-09-21, Task 17): deleting the unwired
# persona/style_mirror.c (zero production callers; casing/punctuation are
# owned by the live style governor hu_daemon_shape_text_inplace) dropped a
# freshly-built measurement from 33 to 32. Auto-lock still does not fire for
# the same reason as above — hand-locking again.
NEVER_LOADED_BASELINE=31   # auto-locked 2026-09-26 (was 32)
# Composition at the baseline: 40 whole function symbols plus 59 function-local
# statics (`_hu_fn.CONSTANT`, `_hu_fn.sql`), which the linker emits as separate
# symbols of the function that owns them. Both are counted, per the plan's
# definition; the statics move with their function, so they are correlated
# rather than independent noise. The FAIL output prints every name.
#
# Re-measured 2026-09-21 alongside NEVER_LOADED_BASELINE above, same config
# change and same reason the auto-lock didn't fire.
DEAD_UNREF_BASELINE=76   # auto-locked 2026-09-26 (was 79)

cd "$_hu_root"

BUILD_DIR="${HU_BUILD_DIR:-build}"
HUMAN_LINK="$BUILD_DIR/CMakeFiles/human.dir/link.txt"
CORE_LINK="$BUILD_DIR/CMakeFiles/human_core.dir/link.txt"

skip() {
    echo "RATCHET_SKIP: $1"
    echo "dead-strip ratchet did not run: $1" >&2
    exit 0
}

# Configuration. The baselines above describe the dev preset's translation-unit
# universe, so a build/ configured any other way measures a different universe
# and its A/B are not comparable to them. That is not hypothetical: on
# 2026-09-26 the main checkout's build/ had been hand-configured with
# HU_ENABLE_ALL_CHANNELS=ON, compiled 1,026 archive members instead of 991, and
# reported A=33 / B=96 against ceilings 31 / 76, failing every push. On a real
# dev-preset build of the same commit (f714418ac) A=31 and B=76.
#
# So compare every cache variable the dev preset sets (resolved through
# `inherits`, the same way scripts/dev/build-options-table.sh does) against
# $BUILD_DIR/CMakeCache.txt, and SKIP on any mismatch. This applies under
# HU_DEAD_STRIP_STRICT=1 as well: strict mode turns off stale-tree demotion
# (a rebuild makes that tree current), but no rebuild can make a
# differently-configured tree into the dev preset. And because it exits before
# ratchet_autolock, a mismatched build can never lock a baseline.
#
# It runs before the platform check so the fixture test
# (tests/fixtures/check-dead-strip-config/) exercises it on Linux CI too.
dev_preset_mismatches() {  # dev_preset_mismatches CACHE_FILE -> "VAR=cached vs preset expected" lines
    python3 - "$1" CMakePresets.json dev <<'PYEOF'
import json, re, sys

cache_path, presets_path, preset_name = sys.argv[1:4]

# Cache variables that do not change which translation units compile.
NOT_FEATURE_SELECTING = {"CMAKE_EXPORT_COMPILE_COMMANDS"}

with open(presets_path) as f:
    by_name = {p["name"]: p for p in json.load(f).get("configurePresets", [])}

def resolved(name, seen=None):
    """Merge cacheVariables along the inherits chain; child overrides parent."""
    seen = seen or set()
    if name in seen or name not in by_name:
        return {}
    seen.add(name)
    preset, merged = by_name[name], {}
    parents = preset.get("inherits") or []
    for parent in parents if isinstance(parents, list) else [parents]:
        merged.update(resolved(parent, seen))
    merged.update(preset.get("cacheVariables", {}))
    return merged

if preset_name not in by_name:
    sys.exit(f"no '{preset_name}' configurePreset in {presets_path}")
expected = resolved(preset_name)

cached = {}
with open(cache_path, errors="replace") as f:
    for line in f:
        m = re.match(r"^([A-Za-z0-9_.+-]+):[A-Z_]+=(.*)$", line.rstrip("\n"))
        if m:
            cached[m.group(1)] = m.group(2)

# CMake's own truthiness (if(<constant>)), so ON vs TRUE vs 1 is not a mismatch.
def as_bool(v):
    u = v.strip().upper()
    if u in ("ON", "YES", "TRUE", "Y") or re.fullmatch(r"[1-9][0-9]*", u):
        return True
    if u in ("OFF", "NO", "FALSE", "N", "0", "IGNORE", "NOTFOUND", "") or u.endswith("-NOTFOUND"):
        return False
    return None

for var, want in sorted(expected.items()):
    if isinstance(want, dict):          # {"type": "BOOL", "value": "ON"}
        want = want.get("value", "")
    want = str(want)
    if var in NOT_FEATURE_SELECTING or "$" in want:   # macros: not comparable
        continue
    got = cached.get(var)
    if got is None:
        print(f"{var}=<unset> vs preset {want}")
        continue
    wb, gb = as_bool(want), as_bool(got)
    same = (wb == gb) if (wb is not None and gb is not None) else got.lower() == want.lower()
    if not same:
        print(f"{var}={got} vs preset {want}")
PYEOF
}

[ -f "$BUILD_DIR/CMakeCache.txt" ] || skip "no $BUILD_DIR/CMakeCache.txt (configure first: cmake --preset dev)"
command -v python3 >/dev/null 2>&1 || \
    skip "python3 not found; cannot confirm $BUILD_DIR is the dev preset the baselines describe"
if ! _mismatches=$(dev_preset_mismatches "$BUILD_DIR/CMakeCache.txt"); then
    skip "could not read the dev preset from CMakePresets.json; cannot confirm $BUILD_DIR matches it"
fi
if [ -n "$_mismatches" ]; then
    # Name at most three: this reason lands in a ratchet-debt-report.sh table
    # cell, and one mismatch is already enough to void the measurement.
    _n=$(printf '%s\n' "$_mismatches" | wc -l | tr -d ' ')
    _shown=$(printf '%s\n' "$_mismatches" | awk 'NR <= 3' | paste -sd ';' - | sed 's/;/; /g')
    [ "$_n" -gt 3 ] && _shown="$_shown; +$((_n - 3)) more"
    skip "$BUILD_DIR is not the dev preset ($_shown); reconfigure with: cmake --preset dev"
fi

# The map parser below is macOS ld's (`# Object files:` / `# Symbols:` /
# `# Dead Stripped Symbols:`). GNU ld's -Map is a different format; until that
# is implemented, say so rather than measuring something else.
[ "$(uname -s)" = "Darwin" ] || skip "unsupported platform ($(uname -s)); macOS ld link maps only"

[ -f "$HUMAN_LINK" ] || skip "no $HUMAN_LINK (configure and build first)"
[ -f "$CORE_LINK" ]  || skip "no $CORE_LINK (configure and build first)"

# Freshness. The anchors are the two LINKED artifacts this gate reads through —
# `human` (whose link line is replayed) and `human_tests` (whose objects supply
# the reference set) — not libhuman_core.a, which can be current while the
# binaries are not. Anything under src/ or include/ newer than either means the
# build dir describes a different tree than the one being committed or pushed.
#
# .m counts: src/tools/vision_ocr_apple.m is compiled into human_core
# (CMakeLists.txt target_sources), so an Objective-C-only edit changes what this
# gate measures. It was invisible to the old '*.[ch]' probe, which reported such
# a tree as fresh and enforced stale counts. The \( \) are required: see the
# matching note in .githooks/pre-push.
for _artifact in human human_tests libhuman_core.a; do
    [ -f "$BUILD_DIR/$_artifact" ] || \
        skip "no $BUILD_DIR/$_artifact (build the human + human_tests targets first)"
done
STALE=0
for _artifact in human human_tests; do
    if [ -n "$(find src include \( -name '*.[ch]' -o -name '*.m' \) -newer "$BUILD_DIR/$_artifact" -print -quit 2>/dev/null)" ]; then
        STALE=1
    fi
done
# Strict mode is the pre-push path, which rebuilds build/ immediately before
# calling this. If it is STILL stale there, the rebuild did not take — enforce
# on what we measured rather than waving the push through, and say so.
if [ "$STALE" = "1" ] && [ "${HU_DEAD_STRIP_STRICT:-0}" = "1" ]; then
    echo "NOTE: $BUILD_DIR still looks older than src/ after the rebuild;" >&2
    echo "      HU_DEAD_STRIP_STRICT=1, so the counts below are enforced anyway." >&2
    STALE=0
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
#
# Counting `path:` headers is the check because nm emits one per file ONLY when
# handed more than one file. That makes the count exact here (1,024 members in
# one or more multi-file xargs batches) and makes the guard also catch the
# nastier variant: if xargs ever splits the list so that a trailing batch holds
# exactly ONE object, nm prints no header for it and its symbols are silently
# attributed to the previous object — headers would then be short by one and
# this comparison fails rather than mis-attributing.
n_members=$(wc -l < "$TMP/members.txt" | tr -d ' ')
n_read=$(grep -c ':$' "$TMP/nm_members.txt" || true)
[ "$n_read" = "$n_members" ] || \
    skip "nm read $n_read of $n_members archive members (stale or moved objects in $BUILD_DIR)"

# Common symbols (nm type C) are EXCLUDED from the liveness test. Under ASan
# every object defines ____asan_globals_registered as a common symbol, and the
# linker coalesces them into one map entry, so testing that name by set
# membership would mark all 981 members live and collapse A to 0. Only
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
        # `NF == 1` alone also admits nm's own `path/to/foo.c.o:` headers as if
        # they were symbols. Harmless for B today (a header can never equal an
        # `_hu_` name), but it pollutes a set whose whole job is membership
        # testing, so drop them.
        #
        # Publish the cache ONLY on success: a partial reference set written
        # under a valid key is worse than no cache, because every later run
        # reuses it and B silently inflates.
        if xargs nm -u < "$TMP/testobjs.txt" 2>/dev/null \
               | awk 'NF == 1 && $1 !~ /:$/ { print $1 }' | sort -u > "$TESTREFS.$$"; then
            mv -f "$TESTREFS.$$" "$TESTREFS"
        else
            rm -f "$TESTREFS.$$"
            skip "nm -u over the test objects failed; refusing to cache a partial reference set"
        fi
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
    echo "NOTE: $BUILD_DIR predates the current sources, so these counts are advisory" >&2
    echo "      and nothing is locked. Rebuild to enforce (and to let the baselines" >&2
    echo "      ratchet down): cmake --build $BUILD_DIR --target human human_tests" >&2
    echo "      The pre-push hook does that rebuild for you before it enforces." >&2
    exit 0
fi

# Auto-lock can ONLY fire from .githooks/pre-commit: scripts/lib/ratchet.sh
# refuses to rewrite a constant unless HU_RATCHET_FROM_HOOK=1, which only that
# hook exports (pre-push cannot — its `git add` would stage a file into no
# commit). So the honest advice is "it locks at your next commit", not the
# sibling ratchets' "lower it by hand", which is the step nobody performed.
lock_note() {  # lock_note NOUN VALUE VAR
    [ "${HU_RATCHET_LOCKED:-0}" = 1 ] && return 0
    echo "NOTE: $1 dropped to $2 — $3 locks to it on the next commit that stages" >&2
    echo "      a src/ change with build/ freshly built (.githooks/pre-commit)." >&2
    return 0
}

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
    lock_note "never-loaded members" "$A" NEVER_LOADED_BASELINE
fi

if [ "$B" -gt "$DEAD_UNREF_BASELINE" ]; then
    echo "FAIL: unreferenced dead symbols grew. Baseline: $DEAD_UNREF_BASELINE, current: $B" >&2
    echo "      Each symbol below is dead-stripped from human and referenced by no test:" >&2
    sed 's/^/  /' "$TMP/dead_unref.txt" >&2
    echo "      Delete it, or give it a caller — see .claude/rules/dead-strip-ratchet.md." >&2
    fail=1
elif [ "$B" -lt "$DEAD_UNREF_BASELINE" ]; then
    lock_note "unreferenced dead symbols" "$B" DEAD_UNREF_BASELINE
fi

exit $fail
