#!/usr/bin/env bash
# check-devlib-flags.sh
#
# human_devlib must compile with EXACTLY the flags human_core_test compiles
# with. This gate diffs the two generated flags.make files and fails on any
# difference.
#
# USAGE: bash scripts/check-devlib-flags.sh
#   HU_BUILD_DIR=build2 bash scripts/check-devlib-flags.sh   # alternate build dir
#
# WHY THIS EXISTS
#
# Task 12 split the test/eval/SDK modules the daemon never links out of
# human_core into a new human_devlib archive. Those modules are selected by
# the same HU_ENABLE_* options as the rest of the tree, so human_devlib has
# to be handed human_core_test's compile definitions, includes and flags —
# otherwise a module gets SELECTED by `if(HU_ENABLE_X)` and then COMPILED
# without -DHU_ENABLE_X=1.
#
# The first implementation mirrored them with get_target_property(). That
# reads a property as it stands at that line, and eight more
# target_compile_definitions / target_include_directories calls on
# human_core_test run further down CMakeLists.txt. So human_devlib compiled
# with 60 defines against human_core_test's 65, silently missing
# HU_ENABLE_RL_FULL=1, HU_EVAL_JUDGE_HAVE_{APPLE_FM,GEMINI_NANO}_IMPL=1 and
# HU_GRPO_HAVE_{HUML,MLX}_IMPL=1 — most sharply, stock_baseline.c is selected
# by `if(HU_ENABLE_RL_FULL)` and was then compiled without it. Commit
# 45d98a4d6 replaced all three with $<TARGET_PROPERTY:human_core_test,...>
# generator expressions, evaluated at generate time after the whole file has
# run, so they cannot miss a later addition.
#
# Nothing stopped someone from reintroducing the same partial mirror. A
# silently-partial mirror defeats the entire reason the mirror exists instead
# of a restatement, and it is invisible: the build is green, the suite is
# green, and the only symptom is a module compiled with the wrong feature
# set. This gate makes it loud. See .claude/rules/reports-success-does-nothing.md.
#
# WHY IT SKIPS RATHER THAN GUESSES
#
# The gate reads a BUILD DIRECTORY, not source text. No build dir, or a
# preset that does not configure human_devlib, means there is nothing to
# measure — and a gate that cannot measure must not block a commit or a
# push. It says so on one line and exits 0, the same contract
# check-dead-strip-ratchet.sh follows. What it will NOT do is pass
# vacuously: if a key it is supposed to compare is absent from BOTH files,
# the flags.make format has changed underneath it and it fails rather than
# reporting "no differences" about a comparison it never made.
set -euo pipefail

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

BUILD_DIR="${HU_BUILD_DIR:-build}"
REF_TARGET="human_core_test"
SUT_TARGET="human_devlib"
REF_FLAGS="$BUILD_DIR/CMakeFiles/$REF_TARGET.dir/flags.make"
SUT_FLAGS="$BUILD_DIR/CMakeFiles/$SUT_TARGET.dir/flags.make"

skip() {
    echo "DEVLIB_FLAGS_SKIP: $1"
    exit 0
}

[ -d "$BUILD_DIR" ] || skip "no $BUILD_DIR/ (configure a preset to enable this gate)"
[ -f "$REF_FLAGS" ] || skip "no $REF_FLAGS ($REF_TARGET not configured or not built)"
[ -f "$SUT_FLAGS" ] || skip "no $SUT_FLAGS ($SUT_TARGET not configured or not built)"

# Read one `KEY = value` assignment. Prints nothing and returns 1 when absent.
flag_value() {
    local file="$1" key="$2" line
    line=$(grep -m1 "^$key = " "$file" 2>/dev/null) || return 1
    printf '%s' "${line#"$key = "}"
}

# The three keys the mirror sets, plus any per-architecture C_FLAGS<arch>
# variant CMake emitted (multi-arch builds get C_FLAGSarm64 alongside
# C_FLAGS; a divergence there is the same bug wearing a different name).
keys="C_DEFINES C_INCLUDES C_FLAGS"
for f in "$REF_FLAGS" "$SUT_FLAGS"; do
    while IFS= read -r k; do
        case " $keys " in *" $k "*) ;; *) keys="$keys $k" ;; esac
    done < <(grep -oE '^C_FLAGS[A-Za-z0-9_]+' "$f" 2>/dev/null || true)
done

fail=0
for key in $keys; do
    ref_missing=0; sut_missing=0
    ref=$(flag_value "$REF_FLAGS" "$key") || ref_missing=1
    sut=$(flag_value "$SUT_FLAGS" "$key") || sut_missing=1

    if [ "$ref_missing" = 1 ] && [ "$sut_missing" = 1 ]; then
        echo "FAIL: $key absent from BOTH flags.make files." >&2
        echo "      This gate cannot compare a key that is not there, and it will not" >&2
        echo "      report 'no differences' about a comparison it never made. The" >&2
        echo "      flags.make format has changed — fix this script." >&2
        fail=1
        continue
    fi
    if [ "$ref_missing" != "$sut_missing" ]; then
        echo "FAIL: $key present in one target and absent from the other." >&2
        echo "      $REF_TARGET: $([ "$ref_missing" = 1 ] && echo ABSENT || echo present)" >&2
        echo "      $SUT_TARGET: $([ "$sut_missing" = 1 ] && echo ABSENT || echo present)" >&2
        fail=1
        continue
    fi

    if [ "$ref" = "$sut" ]; then
        echo "$key: identical"
        continue
    fi

    fail=1
    echo "FAIL: $key differs between $REF_TARGET and $SUT_TARGET." >&2
    # Exact-string mismatch is the contract (include ORDER is semantic), but a
    # set diff is what a reader can act on — that is how the 60-vs-65 define
    # gap reads at a glance.
    only_ref=$(comm -23 <(printf '%s\n' $ref | sort -u) <(printf '%s\n' $sut | sort -u) | tr '\n' ' ')
    only_sut=$(comm -13 <(printf '%s\n' $ref | sort -u) <(printf '%s\n' $sut | sort -u) | tr '\n' ' ')
    [ -n "${only_ref// /}" ] && echo "      only in $REF_TARGET: $only_ref" >&2
    [ -n "${only_sut// /}" ] && echo "      only in $SUT_TARGET: $only_sut" >&2
    if [ -z "${only_ref// /}" ] && [ -z "${only_sut// /}" ]; then
        echo "      same tokens, different ORDER (significant for -I search order)" >&2
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "" >&2
    echo "human_devlib must mirror $REF_TARGET's flags exactly. Use" >&2
    echo "\$<TARGET_PROPERTY:$REF_TARGET,...> generator expressions in CMakeLists.txt," >&2
    echo "never get_target_property() — see commit 45d98a4d6 and this script's header." >&2
    exit 1
fi

echo "OK: $SUT_TARGET mirrors $REF_TARGET's compile flags ($(printf '%s\n' $keys | wc -w | tr -d ' ') keys checked)"
