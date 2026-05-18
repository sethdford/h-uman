#!/usr/bin/env bash
# scripts/check-no-date-time-macros.sh
#
# US-42.3 AC-42.3.1 fast gate: ensure no source file used by the `human`
# binary's release build references __DATE__, __TIME__, or __TIMESTAMP__.
# These macros embed the build wall-clock into the binary and defeat
# reproducible builds. A grep of src/ today returns zero hits — this gate
# prevents regressions.
#
# Strategy: read build/compile_commands.json (jq) and collect the unique
# source files that compose the human binary's translation units, then
# grep each source file for the three banned identifiers as whole words.
# Comments (// ... and /* ... */) are stripped before grep so they don't
# count; string literals are NOT stripped because writing `"__DATE__"` in
# code is still strongly suspect.
#
# Why scan source instead of preprocessed output: the macros are expanded
# by the preprocessor INTO string literals like "May 18 2026", which makes
# them invisible to a regex after `-E`. Scanning the source pre-macro-
# expansion is the only reliable approach. Comment stripping handles
# benign references in doc comments.
#
# The script exits 0 on a clean run, 1 on the first banned-macro hit with a
# file:line citation, and 2 on a script-internal error (missing jq, missing
# compile_commands.json, etc.).
#
# Usage:
#   bash scripts/check-no-date-time-macros.sh                # auto-detect build dir
#   bash scripts/check-no-date-time-macros.sh --build <dir>  # explicit build dir
#
# Local smoke test (verifies the gate actually fires):
#   1. Add `static const char *_probe = __DATE__;` to any src/*.c
#   2. Reconfigure: cmake --preset release
#   3. Run this script; expect exit 1 with that file cited
#   4. Revert the probe
# Do NOT commit the probe fixture.
#
# CI prerequisite: a `release` preset configure has run so that
# build-release/compile_commands.json exists. The release preset is
# CMAKE_BUILD_TYPE=MinSizeRel, which is the configuration where
# -Werror=date-time is wired (see CMakeLists.txt, US-42.3 block). The dev
# preset does NOT enable -Werror=date-time, so this script is the only gate
# in non-release configurations.
#
# Related: sprints/sprint-42/designs/US-42.3.md (Step 4),
# .claude/rules/quality-gates.md (silent failures forbidden).

set -euo pipefail

BUILD_DIR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --build)
            BUILD_DIR="${2:-}"
            shift 2
            ;;
        --build=*)
            BUILD_DIR="${1#--build=}"
            shift
            ;;
        -h|--help)
            sed -n '2,45p' "$0"
            exit 0
            ;;
        *)
            echo "ERROR: unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

# Auto-detect build dir: prefer build-release (CI), fall back to build (dev).
if [ -z "$BUILD_DIR" ]; then
    if [ -f "build-release/compile_commands.json" ]; then
        BUILD_DIR="build-release"
    elif [ -f "build/compile_commands.json" ]; then
        BUILD_DIR="build"
    else
        echo "ERROR: no compile_commands.json found in build-release/ or build/" >&2
        echo "  Run: cmake --preset release" >&2
        exit 2
    fi
fi

CC_JSON="${BUILD_DIR}/compile_commands.json"
if [ ! -f "$CC_JSON" ]; then
    echo "ERROR: $CC_JSON not found" >&2
    exit 2
fi

if ! command -v jq >/dev/null 2>&1; then
    echo "ERROR: jq is required (apt: jq, brew: jq)" >&2
    exit 2
fi

echo "Reproducible-build fast gate: scanning for __DATE__ / __TIME__ / __TIMESTAMP__ ..."
echo "  compile_commands: $CC_JSON"

# Collect unique source files under src/ from compile_commands.json. The
# release preset's compile_commands ONLY contains TUs that go into the
# release binary (human + human_core); tests, fuzzers, and wasm TUs are
# in separate build dirs.
mapfile -t source_files < <(
    jq -r '.[] | .file' "$CC_JSON" \
        | grep -E '/src/[^/]+\.c$|/src/.*\.c$' \
        | sort -u
)

if [ "${#source_files[@]}" -eq 0 ]; then
    echo "ERROR: no src/*.c files found in $CC_JSON — wrong build dir?" >&2
    exit 2
fi

violations=0
checked=0

for src in "${source_files[@]}"; do
    if [ ! -f "$src" ]; then
        # Out-of-tree TU (generated, etc.) — skip silently.
        continue
    fi

    checked=$((checked + 1))

    # Strip /* ... */ block comments and // ... line comments before the
    # grep so doc-comments referencing these macros don't trip the gate.
    # We use a small awk that:
    #   - strips // ... to end-of-line
    #   - tracks /* ... */ across lines and blanks them out
    # This is approximate (no full C tokenizer) but plenty for our policy.
    stripped=$( awk '
        BEGIN { in_block = 0 }
        {
            line = $0
            out = ""
            i = 1
            n = length(line)
            while (i <= n) {
                if (in_block) {
                    # look for */
                    p = index(substr(line, i), "*/")
                    if (p == 0) { i = n + 1; break }
                    i = i + p + 1
                    in_block = 0
                    continue
                }
                # not in block
                c1 = substr(line, i, 1)
                c2 = substr(line, i, 2)
                if (c2 == "/*") {
                    in_block = 1
                    i = i + 2
                    continue
                }
                if (c2 == "//") {
                    i = n + 1
                    break
                }
                out = out c1
                i = i + 1
            }
            print out
        }
    ' "$src" )

    if hits=$( printf '%s\n' "$stripped" \
                | grep -nE '\b(__DATE__|__TIME__|__TIMESTAMP__)\b' \
                || true ); [ -n "$hits" ]; then
        violations=$((violations + 1))
        echo "FAIL: $src expands banned date/time macro(s):" >&2
        echo "$hits" | head -5 | sed 's/^/    /' >&2
    fi
done

if [ "$violations" -gt 0 ]; then
    echo "" >&2
    echo "FAIL: $violations source file(s) reference banned date/time macros." >&2
    echo "  These break reproducible builds. Replace with SOURCE_DATE_EPOCH-aware" >&2
    echo "  constants if a timestamp is genuinely required." >&2
    exit 1
fi

echo "OK: $checked source file(s) scanned; no __DATE__ / __TIME__ / __TIMESTAMP__ usage."
exit 0
