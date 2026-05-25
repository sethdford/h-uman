#!/bin/sh
# scripts/check-test-silent-pass.sh
#
# Detects the "tests that pin bugs" anti-pattern: a test whose primary
# assertion is gated on the existence of the THING IT'S SUPPOSED TO
# VERIFY, so the test silently PASSES when that thing is missing.
#
# Documented at: .claude/rules/tests-that-pin-bugs.md
#
# Recurred 3 times in recent sprints (US-C1.1, US-C1.2, US-C1.4):
#   if (file_exists(binary_path)) {     <-- silent gate
#       HU_ASSERT(file_is_executable(binary_path));
#   }
#
# Correct shape:
#   HU_SKIP_IF(!file_exists(binary_path), "build artifact required");
#   HU_ASSERT(file_is_executable(binary_path));
#
# Or:
#   HU_ASSERT_NOT_NULL(fopen(path, "r"));  // hard fail if missing
#
# Usage:
#   scripts/check-test-silent-pass.sh                 # checks staged test files
#   scripts/check-test-silent-pass.sh tests/test_X.c  # check specific file(s)
#
# Exit codes:
#   0 — no anti-pattern found
#   1 — anti-pattern detected (commit blocked)

set -eu

# Patterns that indicate "this is checking whether the thing under
# test exists" — usually the wrong shape inside a test gate.
# (Pattern intentionally without unbalanced parens — awk's regex
# parser rejects them. Use word-roots; the awk match below tolerates
# either word_exists or word_exists() in the source.)
SUSPICIOUS_GATE_CALLS='file_exists|path_exists|path_exists_and_nonempty|access[(]|stat[(]|fopen|is_executable|file_is_executable'

# Files to inspect. Default = staged test files; CLI args override.
if [ $# -gt 0 ]; then
    FILES="$*"
else
    FILES="$(git diff --cached --name-only --diff-filter=ACM -- 'tests/test_*.c' 2>/dev/null || true)"
fi

if [ -z "$FILES" ]; then
    exit 0
fi

FAIL=0
for f in $FILES; do
    [ -f "$f" ] || continue
    # Look for `if (<suspicious-call>...)` followed within 10 lines by
    # an HU_ASSERT (any flavor). awk lets us span lines reliably.
    HITS=$(awk -v pat="$SUSPICIOUS_GATE_CALLS" '
        BEGIN { gate=0; gate_line=0; gate_text="" }
        /\/\// && /allow-silent-pass/ { gate=0; next }   # opt-out comment
        {
            line=$0
            if (match(line, "if[[:space:]]*[(].*(" pat ")")) {
                gate=1
                gate_line=NR
                gate_text=$0
                next
            }
            if (gate > 0) {
                gate++
                if (match(line, "HU_ASSERT")) {
                    printf("%s:%d: silent-pass antipattern — assertion gated on %s\n  gate: %s\n  assert: %s\n", FILENAME, gate_line, "(see .claude/rules/tests-that-pin-bugs.md)", gate_text, $0)
                    gate=0
                    next
                }
                if (gate > 12) { gate=0 }  # window expired
            }
        }
    ' "$f")
    if [ -n "$HITS" ]; then
        echo "$HITS" >&2
        FAIL=1
    fi
done

if [ "$FAIL" -ne 0 ]; then
    cat >&2 <<'EOF'

❌ tests-that-pin-bugs antipattern detected.

A test's primary assertion is gated on whether the input-under-test
exists — meaning the test silently PASSES when the input is missing.

Fix options:
  (a) HU_SKIP_IF(!check, "reason") — honest skip when prerequisite missing
  (b) HU_ASSERT_NOT_NULL(fopen(...)) — hard fail if input missing
  (c) Make the prerequisite a CMake test dependency / fixture

If this gate is INTENTIONALLY a soft check (rare — usually the test is
the wrong shape), add `// allow-silent-pass` on the same line as the gate.

See .claude/rules/tests-that-pin-bugs.md for the full rule.
EOF
    exit 1
fi

exit 0
