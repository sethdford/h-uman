#!/bin/sh
# scripts/check-disabled-test-registration.sh
#
# Detects the "hollow green via disabled test" anti-pattern: a test function
# is DEFINED but its HU_RUN_TEST(...) registration is COMMENTED OUT, so the
# suite reports all-green while silently not exercising that test.
#
# Documented at: .claude/rules/tests-that-pin-bugs.md (sibling anti-pattern).
#
# Real incident (Sprint 60, US-101): an implementer commented out the two
# hardest acceptance-criteria tests with a vague excuse and reported DONE:
#
#     // HU_RUN_TEST(test_preference_ranking_5_seeds);  /* Deferred: debug allocator init */
#     // HU_RUN_TEST(test_training_reduces_loss);      /* Deferred: debug allocator init */
#
# The suite passed (the two functions were just dead -Wunused-function), but
# AC-101.3 and AC-101.5 — the entire point of the reward model — were not
# tested. Only a manual read caught it. This guard makes it a hard CI failure.
#
# Correct shapes (any of):
#   - Actually register the test:  HU_RUN_TEST(test_foo);
#   - Delete the test if obsolete (don't leave a commented corpse).
#   - For a genuinely-deferred test, gate the whole suite/feature properly
#     (see .claude/rules/test-source-gate-symmetry.md) rather than commenting
#     out one registration line.
#
# Rare legitimate exception: add `// allow-disabled-test: <reason>` on the
# same line. Use sparingly — a disabled test is a coverage hole.
#
# Usage:
#   scripts/check-disabled-test-registration.sh                 # staged test files
#   scripts/check-disabled-test-registration.sh tests/test_X.c  # specific file(s)
#
# Exit codes:
#   0 — no disabled registrations found
#   1 — disabled HU_RUN_TEST registration detected (commit blocked)

set -eu

# Files to inspect. Default = staged test files; CLI args override.
if [ $# -gt 0 ]; then
    files="$*"
else
    files=$(git diff --cached --name-only --diff-filter=ACM 2>/dev/null \
        | grep -E '^tests/test_.*\.c$' || true)
fi

[ -z "$files" ] && exit 0

found=0
for f in $files; do
    [ -f "$f" ] || continue
    # Match lines that are a COMMENTED-OUT HU_RUN_TEST registration:
    #   optional leading ws, // (line comment), optional ws, HU_RUN_TEST(
    # Block-comment variants (/* ... HU_RUN_TEST */) are caught too via the
    # leading-comment-marker alternation. Lines carrying the opt-out marker
    # are skipped.
    while IFS= read -r line; do
        # An empty command substitution still feeds one blank line through the
        # heredoc — skip it so "no matches" doesn't read as a finding.
        [ -z "$line" ] && continue
        case "$line" in
            *allow-disabled-test*) continue ;;
        esac
        echo "  $f: $line" | sed 's/[[:space:]]*$//'
        found=1
    done <<EOF
$(grep -nE '^[[:space:]]*(//|/\*)[[:space:]]*HU_RUN_TEST[[:space:]]*\(' "$f" 2>/dev/null || true)
EOF
done

if [ "$found" -eq 1 ]; then
    echo "ERROR: commented-out HU_RUN_TEST registration(s) found above." >&2
    echo "A defined-but-unregistered test is a silent coverage hole (the suite" >&2
    echo "goes green without running it). Either register it, delete it, or — if" >&2
    echo "truly deferred — add '// allow-disabled-test: <reason>' on that line." >&2
    echo "See .claude/rules/tests-that-pin-bugs.md." >&2
    exit 1
fi

exit 0
