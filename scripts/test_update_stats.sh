#!/usr/bin/env bash
# Tests for update-stats.sh: a fake repo in mktemp, fake test binaries that
# print a Results: line, and assertions that the stamped test count comes
# from the RIGHT measurement (the caller's --test-count first, build-check/
# before build/, nothing at all when no count can be parsed).
#
# Background: three times (2026-07-26, 2026-09-03 twice) the pre-push hook
# verified one tree and update-stats.sh stamped a count read from a stale
# binary in a different one. See .claude/rules/no-number-without-a-measurement.md.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$HERE/update-stats.sh"
fail=0; check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }

# --- fake repo -------------------------------------------------------------
# update-stats.sh cd's to `git rev-parse --show-toplevel`, so the tree must be
# a git repo. Every counter the script runs (find src/ include/ tests/, the
# channel enum grep) needs a real-looking file to count; the values themselves
# are irrelevant to these tests.
T=$(mktemp -d)
git -C "$T" init -q
mkdir -p "$T/src/channels" "$T/src/tools" "$T/include/human" "$T/tests"
echo 'int a;' > "$T/src/a.c"
echo 'int b;' > "$T/src/channels/b.c"
echo 'int c;' > "$T/src/tools/c.c"
printf 'enum {\n    HU_CHANNEL_ONE,\n};\n' > "$T/include/human/channel_catalog.h"
echo 'void test_a(void) {}' > "$T/tests/test_a.c"

# Doc phrasings the script rewrites, at their committed values.
write_docs() {
    printf 'Tests: 999\n~2952 KB\n' > "$T/README.md"
    printf 'Current scale: **old**\n' > "$T/AGENTS.md"
    printf 'All 999+ tests must pass\n' > "$T/CONTRIBUTING.md"
    printf '~2952 KB binary, 999+ tests\n' > "$T/CLAUDE.md"
}

# fake_bin <path> <results-line-or-empty>: a "test binary" that records that
# it ran (so a test can assert it did NOT) and prints the given Results line.
fake_bin() {
    mkdir -p "$(dirname "$1")"; rm -f "$1.ran"
    printf '#!/bin/sh\ntouch "%s.ran"\n' "$1" > "$1"
    [ -z "$2" ] || printf 'echo "%s"\n' "$2" >> "$1"
    chmod +x "$1"
}

run() { (cd "$T" && bash "$SCRIPT" "$@" 2>&1); }

# 1. --test-count wins over a binary that reports a different number, and the
#    binary is never executed (the caller already ran the suite it trusts).
write_docs
fake_bin "$T/build/human_tests" "--- Results: 111/111 passed, 5 skipped ---"
out=$(run --test-count 222 --apply)
check "override stamps the caller's count" "grep -q '^Tests: 222$' '$T/README.md'"
check "override reaches CONTRIBUTING too"  "grep -q 'All 222+ tests must pass' '$T/CONTRIBUTING.md'"
check "override never executes a binary"   "[ ! -e '$T/build/human_tests.ran' ]"

# 2. A non-numeric --test-count is a usage error, not a silent 'unknown'.
write_docs
out=$(run --test-count abc --apply); rc=$?
check "non-numeric --test-count exits 2"  "[ $rc -eq 2 ]"
check "non-numeric --test-count touches nothing" "grep -q '^Tests: 999$' '$T/README.md'"

# 3. A binary with no parseable Results: line leaves every count untouched.
write_docs
fake_bin "$T/build/human_tests" ""
out=$(run --apply); rc=$?
check "unparseable count exits 0"          "[ $rc -eq 0 ]"
check "unparseable count leaves README"    "grep -q '^Tests: 999$' '$T/README.md'"
check "unparseable count leaves CLAUDE.md" "grep -q '999+ tests' '$T/CLAUDE.md'"
check "unparseable count warns"            "[[ \"$out\" == *'WARN'* ]]"

# 4. With no override, build-check/ (what the pre-push hook just built) beats
#    build/ (a developer's dev build of some other tree state), and the script
#    says which binary it ran and how old it is.
write_docs
fake_bin "$T/build/human_tests"       "--- Results: 111/111 passed ---"
fake_bin "$T/build-check/human_tests" "--- Results: 333/333 passed ---"
out=$(run --apply)
check "build-check preferred over build"   "grep -q '^Tests: 333$' '$T/README.md'"
check "reports which binary was run"       "[[ \"$out\" == *'build-check/human_tests'* ]]"
check "reports the binary's mtime"         "[[ \"$out\" == *'mtime'* ]]"
check "build/ binary was not run"          "[ ! -e '$T/build/human_tests.ran' ]"

# 5. --keep-binary-size leaves the committed KB even when a genuine MinSizeRel
#    binary exists — the hook cannot vouch for that binary's age, and a
#    five-week-old release build is what stamped ~2952 KB on 2026-09-03.
write_docs
rm -rf "$T/build" "$T/build-check"
mkdir -p "$T/build-size"
printf 'CMAKE_BUILD_TYPE:STRING=MinSizeRel\nHU_ENABLE_ASAN:BOOL=OFF\n' > "$T/build-size/CMakeCache.txt"
head -c 4096 /dev/zero > "$T/build-size/human"
out=$(run --test-count 222 --keep-binary-size --apply)
check "--keep-binary-size leaves README size"    "grep -q '^~2952 KB$' '$T/README.md'"
check "--keep-binary-size leaves CLAUDE.md size" "grep -q '^~2952 KB binary' '$T/CLAUDE.md'"
check "--keep-binary-size still stamps the count" "grep -q '^Tests: 222$' '$T/README.md'"
# ...and without the flag the same release binary IS measured (4096 B = 4 KB).
write_docs
out=$(run --test-count 222 --apply)
check "release binary measured without the flag" "grep -q '^~4 KB$' '$T/README.md'"

rm -rf "$T"
exit $fail
