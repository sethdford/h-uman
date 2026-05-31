#!/usr/bin/env bash
# predict-tests.sh — header-level predictive test selection.
#
# what-to-test.sh maps a changed file to suites by its own DIRECTORY. That misses
# the real blast radius of a HEADER change: editing include/human/core/json.h can
# break any test whose translation unit transitively #includes it, not just the
# "JSON" suite. This script traces the reverse #include graph from the changed
# headers outward and selects the suites of the test files actually reached.
#
# Honest naming: this is DETERMINISTIC dependency-reachability selection — the
# corpus-free realization of "run the tests most likely to be affected by this
# change." It is NOT ML failure-probability ranking (that needs a historical
# failure corpus this repo does not yet collect); when one exists, this script is
# where the reached suites would be weighted/ordered.
#
# It is ADVISORY (a fast-local-iteration aid like what-to-test.sh), never a gate:
# CI still runs the full suite. Under-selection at worst means a dev misses a
# break locally that CI catches; over-selection just runs extra. When the blast
# radius is large (a hot header, a core change, a loose root .c, the build
# system), it prints "RUN THE FULL SUITE" and emits no filter.
#
# Usage:
#   scripts/predict-tests.sh [file ...]            # emit --suite=… lines (stdout)
#   scripts/predict-tests.sh --explain [file ...]  # also explain selections (stderr)
#   (no files → git diff vs HEAD)
#   ./build/human_tests $(scripts/predict-tests.sh include/human/core/json.h)
#
# bash 3.2 compatible (macOS): no associative arrays.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

EXPLAIN=0
if [ "${1:-}" = "--explain" ]; then
    EXPLAIN=1
    shift
fi
note() { [ "$EXPLAIN" -eq 1 ] && echo "  $*" >&2 || true; }

# Changed files (args, else git diff).
FILES="$([ "$#" -gt 0 ] && printf '%s\n' "$@" || {
    git diff --name-only HEAD 2>/dev/null || true
    git diff --cached --name-only 2>/dev/null || true
})"
FILES="$(printf '%s\n' "$FILES" | sed '/^$/d' | sort -u)"
if [ -z "$FILES" ]; then
    echo "# predict-tests: no changed files detected." >&2
    exit 0
fi

# Seed sets:
#   DIRTY        — header basenames whose change propagates through the include graph
#   DIRECT_SUITES — suites named directly by a changed test file
#   FANOUT       — changed things with unbounded blast radius (run full suite)
DIRTY=""
DIRECT_SUITES=""
FANOUT=""
note "changed files:"
for f in $FILES; do
    note "  $f"
    case "$f" in
    tests/test_*.c)
        b="$(basename "$f" .c)"
        DIRECT_SUITES="${DIRECT_SUITES} ${b#test_}"
        ;;
    *.h)
        DIRTY="${DIRTY} $(basename "$f")"
        ;;
    src/*/*.c)
        # A .c isn't #included by anyone, so it has no reverse-include ripple of
        # its own — but its sibling header (if any) does. Seed that header, and
        # let what-to-test.sh provide the direct suite below.
        sib="include/human/$(printf '%s' "$f" | sed -E 's|^src/(.*)\.c$|\1.h|')"
        [ -f "$sib" ] && DIRTY="${DIRTY} $(basename "$sib")"
        ;;
    src/*.c | CMakeLists.txt | CMakePresets.json)
        # Loose root source or the build system: blast radius is unbounded.
        FANOUT="${FANOUT} ${f}"
        ;;
    esac
done

if [ -n "$(printf '%s' "$FANOUT" | tr -d ' ')" ]; then
    echo "# predict-tests: high-fanout change ($FANOUT) — RUN THE FULL SUITE." >&2
    echo "# (no --suite filter emitted; run ./build/human_tests with no args)" >&2
    exit 0
fi

DIRTY="$(printf '%s\n' $DIRTY | sed '/^$/d' | sort -u | tr '\n' ' ')"

# Fixpoint: a header that #includes a dirty header is itself dirty (the change
# propagates up the include chain). Bounded by include depth (a handful of
# rounds); each round greps the source+include tree once.
make_alt() { printf '%s' "$1" | tr ' ' '\n' | sed '/^$/d; s/\./\\./g' | paste -sd'|' -; }

if [ -n "$(printf '%s' "$DIRTY" | tr -d ' ')" ]; then
    for _round in 1 2 3 4 5 6; do
        alt="$(make_alt "$DIRTY")"
        [ -z "$alt" ] && break
        # Headers that include any currently-dirty header.
        new_hdrs="$(grep -rlE "#include[[:space:]]*\"[^\"]*(${alt})\"" \
            src include --include='*.h' 2>/dev/null |
            xargs -n1 basename 2>/dev/null | sort -u || true)"
        before="$DIRTY"
        DIRTY="$(printf '%s\n%s\n' "$DIRTY" "$new_hdrs" | tr ' ' '\n' | sed '/^$/d' | sort -u | tr '\n' ' ')"
        [ "$DIRTY" = "$before" ] && break
    done
fi
note "dirty headers (transitive): ${DIRTY:-<none>}"

# Test files whose TU includes any dirty header → their suites.
REACHED_SUITES=""
if [ -n "$(printf '%s' "$DIRTY" | tr -d ' ')" ]; then
    alt="$(make_alt "$DIRTY")"
    if [ -n "$alt" ]; then
        reached="$(grep -rlE "#include[[:space:]]*\"[^\"]*(${alt})\"" \
            tests --include='test_*.c' 2>/dev/null || true)"
        for t in $reached; do
            b="$(basename "$t" .c)"
            REACHED_SUITES="${REACHED_SUITES} ${b#test_}"
        done
    fi
fi

ALL_SUITES="$(printf '%s\n' $DIRECT_SUITES $REACHED_SUITES | sed '/^$/d' | sort -u)"
SUITE_COUNT="$(printf '%s\n' "$ALL_SUITES" | sed '/^$/d' | wc -l | tr -d ' ')"
TOTAL_TESTS="$(find tests -name 'test_*.c' | wc -l | tr -d ' ')"

# A selection only helps if it is meaningfully SMALLER than the whole suite.
# Two guards make the output always either a tight, useful filter or an honest
# "full suite" — never a useless list of hundreds of --suite= args:
#   - absolute cap: more than CAP suites is not a "selection" anymore.
#   - relative cap: more than ~60% of all test files is no faster than full.
SELECT_CAP=25
if [ "$SUITE_COUNT" -gt "$SELECT_CAP" ] ||
    { [ "$TOTAL_TESTS" -gt 0 ] && [ "$((SUITE_COUNT * 100 / TOTAL_TESTS))" -gt 60 ]; }; then
    echo "# predict-tests: ${SUITE_COUNT}/${TOTAL_TESTS} suites reached (hot/central header) — RUN THE FULL SUITE." >&2
    echo "# (no --suite filter emitted; run ./build/human_tests with no args)" >&2
    exit 0
fi

if [ -z "$ALL_SUITES" ]; then
    echo "# predict-tests: no test mapping for the changed files — RUN THE FULL SUITE." >&2
    exit 0
fi

printf '%s\n' "$ALL_SUITES" | while IFS= read -r s; do
    [ -n "$s" ] && echo "--suite=$s"
done

if [ "$EXPLAIN" -eq 1 ]; then
    echo "  direct test suites:  ${DIRECT_SUITES:-<none>}" >&2
    echo "  reached via includes: ${REACHED_SUITES:-<none>}" >&2
    echo "  total suites: ${SUITE_COUNT}/${TOTAL_TESTS} test files" >&2
fi
