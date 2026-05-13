#!/usr/bin/env bash
# Smoke test: the guard script exists, is executable, and exits non-zero
# when fed a synthetic file with a hardcoded filler, and exits zero when
# no hardcoded fillers are present.
#
# This is a developer-runnable shell test (not a C test); it does not
# participate in the human_tests binary or test_main.c registry.
# Run manually: bash tests/test_filler_guard.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

GUARD="$REPO_ROOT/tools/check-no-hardcoded-fillers.sh"

# Precondition: guard script exists and is executable
if [ ! -x "$GUARD" ]; then
    echo "FAIL: guard script not executable: $GUARD" >&2
    exit 1
fi

# Test 1: with a hardcoded filler present, guard must exit non-zero
echo "static const char *x = \"ooh that's a tough one\";" > "$TMP/test.c"
mkdir -p "$TMP/src"
mv "$TMP/test.c" "$TMP/src/test.c"
cp -r "$REPO_ROOT/tools" "$TMP/"
if ( cd "$TMP" && bash tools/check-no-hardcoded-fillers.sh ) 2>/dev/null; then
    echo "FAIL: guard should have exited non-zero when filler present" >&2
    exit 1
fi
echo "Test 1 passed: guard correctly detected hardcoded filler"

# Test 2: with no hardcoded fillers, guard must exit zero
rm -rf "$TMP/src"
mkdir -p "$TMP/src"
echo "static const char *x = \"hello world\";" > "$TMP/src/test.c"
if ! ( cd "$TMP" && bash tools/check-no-hardcoded-fillers.sh ); then
    echo "FAIL: guard should have exited 0 when no filler present" >&2
    exit 1
fi
echo "Test 2 passed: guard correctly passed with no hardcoded filler"

# Test 3: HU_SKIP_FILLER_GUARD=1 must bypass even when filler present
rm -rf "$TMP/src"
mkdir -p "$TMP/src"
echo "static const char *x = \"let me think about that for a sec\";" > "$TMP/src/test.c"
if ! ( cd "$TMP" && HU_SKIP_FILLER_GUARD=1 bash tools/check-no-hardcoded-fillers.sh ); then
    echo "FAIL: guard should have exited 0 with HU_SKIP_FILLER_GUARD=1" >&2
    exit 1
fi
echo "Test 3 passed: HU_SKIP_FILLER_GUARD=1 bypass works"

echo "test_filler_guard: OK"
