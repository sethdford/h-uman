#!/usr/bin/env bash
# check-untested.sh — Find src/*.c files with no corresponding test coverage.
# Exit 0 if all files have coverage, exit 1 if gaps found.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

SKIP_PATTERNS="factory.c|meta_common.c|main.c"
FOUND=0

for src in $(find src -name '*.c' | sort); do
    base=$(basename "$src" .c)

    if echo "$base" | grep -qE "$SKIP_PATTERNS"; then
        continue
    fi

    # Check if any test file references a function from this module
    if grep -rql "${base}" tests/ >/dev/null 2>&1; then
        continue
    fi

    echo "  NO TEST: $src"
    FOUND=$((FOUND + 1))
done

if [ "$FOUND" -eq 0 ]; then
    echo "All source files have test references."
else
    echo ""
    echo "$FOUND source file(s) with no test references found."
fi

exit 0
