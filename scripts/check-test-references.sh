#!/usr/bin/env bash
# check-test-references.sh — Verify that test files reference at least one production symbol.
#
# Rule: a test file matching tests/test_*.c must contain at least one grep-detectable
# reference to a hu_* function or macro exported from the corresponding production .c
# file (src/**/<module>.c). This prevents the "test inlines production code" anti-pattern
# where a test file reimplements the logic it claims to test.
#
# Escape hatch: add a line containing exactly "// @covers-none" to the test file to
# suppress this check for genuine cross-module or header-only tests.
#
# Exit codes:
#   0  — all checked test files reference at least one production symbol (or opted out)
#   1  — one or more test files reference no production symbol from their implied module
#
# Usage:
#   # Check specific files (passed as arguments):
#   check-test-references.sh tests/test_foo.c tests/test_bar.c
#
#   # Check all staged new/modified test files (git pre-commit mode):
#   check-test-references.sh
#
#   # Show this help:
#   check-test-references.sh --help

set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

if [[ "${1-}" == "--help" ]]; then
    cat <<'HELP'
check-test-references.sh — Verify test files reference at least one production symbol.

Rule: a test file matching tests/test_*.c must contain at least one grep-detectable
reference to a hu_* function exported from the corresponding production .c file
(src/**/<module>.c). This prevents "test inlines production code" anti-pattern.

Escape hatch: add a line containing "// @covers-none" to the test file to suppress
this check for genuine cross-module or header-only tests.

Exit codes:
  0  — all checked test files reference at least one production symbol (or opted out)
  1  — one or more test files reference no production symbol from their implied module

Usage:
  check-test-references.sh tests/test_foo.c tests/test_bar.c  # explicit files
  check-test-references.sh                                     # staged files (pre-commit mode)
  check-test-references.sh --help                              # this message
HELP
    exit 0
fi

# ── Determine which test files to check ───────────────────────────────────────

EXPLICIT_FILES=false
if [[ $# -gt 0 ]]; then
    # Explicit file list passed by caller (e.g. from the pre-commit hook for staged files,
    # or manual spot-check runs such as smoke-testing fixture files).
    FILES=("$@")
    EXPLICIT_FILES=true
else
    # Default: all staged new/modified test files (pre-commit mode).
    mapfile -t FILES < <(git diff --cached --name-only --diff-filter=AM -- 'tests/test_*.c' 2>/dev/null || true)
fi

if [[ ${#FILES[@]} -eq 0 ]]; then
    exit 0
fi

# ── Helper: derive the candidate production module from a test filename ────────
#
# Strategy: strip the "tests/test_" prefix and ".c" suffix, then try progressively
# shorter prefixes until we find at least one matching src/**/<candidate>.c.
# Example: tests/test_daemon_e2e_validator.c → tries "daemon_e2e_validator", then
# "daemon_e2e", then "daemon" → finds src/daemon.c → uses daemon.
#
# Disambiguation when MULTIPLE candidates share a basename (e.g. both
# src/channels/imessage.c and src/feeds/imessage.c exist): score each candidate
# by how many of its exported hu_* symbols are referenced in the test file, and
# pick the one with the highest score. Ties (including all-zero) broken
# alphabetically by full path — deterministic across filesystems, unlike the
# previous `find ... | head -1` which was filesystem-order-dependent.
#
# Why this matters: before the fix, a test legitimately covering
# src/channels/imessage.c symbols could fail because `find` happened to return
# src/feeds/imessage.c first, and the script demanded symbols from the wrong
# module. Authors then over-used the @covers-none escape hatch.
#
# Returns the path of the picked file, or empty string.

find_production_module() {
    local test_file="$1"
    local base
    base="$(basename "$test_file" .c)"     # e.g. test_daemon_e2e_validator
    base="${base#test_}"                    # e.g. daemon_e2e_validator

    local candidate="$base"
    while [[ -n "$candidate" ]]; do
        # Collect ALL matching src files, sorted by full path for deterministic
        # tie-breaking. `sort` here means filesystem order can never affect the
        # result.
        local matches
        mapfile -t matches < <(find src -name "${candidate}.c" -type f | sort)

        if [[ ${#matches[@]} -eq 1 ]]; then
            # Single candidate: fast path, no scoring needed.
            echo "${matches[0]}"
            return
        fi

        if [[ ${#matches[@]} -gt 1 ]]; then
            # Multiple candidates: score each by how many of its hu_* symbols
            # appear in the test file. Highest score wins; ties broken by the
            # alphabetical sort above. Score 0 for all = first alphabetical
            # (preserves the prior fallback behavior).
            local best=""
            local best_score=-1
            local m sym score
            for m in "${matches[@]}"; do
                score=0
                while IFS= read -r sym; do
                    if [[ -n "$sym" ]] && grep -qF "$sym" "$test_file" 2>/dev/null; then
                        score=$((score + 1))
                    fi
                done < <(extract_production_symbols "$m")

                if [[ $score -gt $best_score ]]; then
                    best_score=$score
                    best="$m"
                fi
            done
            echo "$best"
            return
        fi

        # No matches for this candidate — strip last underscore segment and retry.
        local shorter="${candidate%_*}"
        [[ "$shorter" == "$candidate" ]] && break
        candidate="$shorter"
    done
    echo ""
}

# ── Helper: extract hu_* symbol names defined in a production .c file ─────────
#
# Extracts hu_* function names from a C source file.  Two patterns are tried:
#   1. Lines where hu_* is the first identifier (return type on preceding line or
#      the function name starts the line): "^hu_<name>("
#   2. Lines where hu_* appears after a return type: any "hu_[a-z_]+(" occurrence
# We do not require a full C parser — false negatives are acceptable; false positives
# would only cause spurious passes.

extract_production_symbols() {
    local src_file="$1"
    # Match hu_* identifiers followed by ( anywhere on a line, then strip trailing (.
    grep -oE 'hu_[a-z_][a-z_0-9]*[[:space:]]*\(' "$src_file" \
        | sed 's/[[:space:]]*($//' \
        | sort -u
}

# ── Main check loop ────────────────────────────────────────────────────────────

FAIL=0

for test_file in "${FILES[@]}"; do
    # Normalise to relative path from repo root
    test_file="${test_file#"$REPO_ROOT/"}"

    # In default (pre-commit) mode only process test_*.c files in tests/.
    # When files are passed explicitly the caller is being intentional — skip
    # the path filter so fixture files (e.g. tests/fixtures/check-test-refs/bad.c)
    # can be used for smoke-testing the script itself.
    if [[ "$EXPLICIT_FILES" == "false" ]]; then
        case "$test_file" in
            tests/test_*.c) ;;
            *) continue ;;
        esac
    fi

    if [[ ! -f "$test_file" ]]; then
        continue
    fi

    # Escape hatch: test file explicitly opts out
    if grep -q '@covers-none' "$test_file" 2>/dev/null; then
        continue
    fi

    # Find the corresponding production module
    prod_file="$(find_production_module "$test_file")"

    if [[ -z "$prod_file" ]]; then
        # No matching source file found by filename. Fall back to a content scan:
        # the test must reference SOME hu_* symbol from production. This catches
        # cross-module tests that genuinely exercise production code while still
        # blocking tests with no production references at all (the original
        # Sprint 3 "inline-copied logic with no production function call"
        # anti-pattern). See sprints/sprint-4/critic.md MED finding.
        if grep -qE 'hu_[a-z_][a-z_0-9]*[[:space:]]*\(' "$test_file"; then
            continue
        fi
        echo "FAIL  $test_file: no production module found by filename and no hu_* symbol referenced." >&2
        echo "      Add '// @covers-none' if this is intentionally standalone, or reference at least one production function." >&2
        FAIL=1
        continue
    fi

    # Extract production symbols
    mapfile -t symbols < <(extract_production_symbols "$prod_file")

    if [[ ${#symbols[@]} -eq 0 ]]; then
        # Production file has no extractable hu_* symbols (e.g. static-only).  Pass.
        continue
    fi

    # Check whether the test file references at least one production symbol
    found=0
    for sym in "${symbols[@]}"; do
        if grep -qF "$sym" "$test_file" 2>/dev/null; then
            found=1
            break
        fi
    done

    if [[ $found -eq 0 ]]; then
        preview="${symbols[*]:0:5}"
        more=""
        [[ ${#symbols[@]} -gt 5 ]] && more=" ..."
        echo "FAIL  $test_file: references no production symbol from $prod_file" >&2
        echo "      Expected at least one of: $preview$more" >&2
        echo "      If this test is intentionally standalone, add '// @covers-none' to suppress." >&2
        FAIL=$((FAIL + 1))
    fi
done

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi

exit 0
