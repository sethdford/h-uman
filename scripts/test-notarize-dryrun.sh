#!/usr/bin/env bash
# test-notarize-dryrun.sh — adversarial test for scripts/notarize-mac.sh
# in --dry-run mode. Stubs xcrun on PATH; the stub LOUDLY fails on
# submit/staple/log so that any accidental real-mode network call by the
# script under test trips a recorded failure.
#
# Asserts the NEGATIVE per .claude/rules/tests-that-pin-bugs.md:
#     "the stub xcrun submit/staple/log was NOT invoked"
# NOT the positive "submit was invoked with X args" (which would pin the
# bug we're guarding against: dry-run accidentally calling the network).
#
# Exit codes:
#   0 — dry-run path validated, xcrun submit/staple/log never invoked
#   1 — test setup or fixture problem
#   2 — assertion failed: xcrun network/staple subcommand WAS invoked
#       (dry-run regression)
#   3 — assertion failed: dry-run didn't produce expected DMG / marker

set -euo pipefail

THIS_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$THIS_SCRIPT_DIR/.." && pwd)"
NOTARIZE="$REPO_ROOT/scripts/notarize-mac.sh"
FIXTURE_APP="$REPO_ROOT/tests/fixtures/notarize/Human.app"

if [[ ! -x "$NOTARIZE" ]]; then
    echo "test-notarize-dryrun: scripts/notarize-mac.sh not found or not executable: $NOTARIZE" >&2
    exit 1
fi
if [[ ! -d "$FIXTURE_APP" ]]; then
    echo "test-notarize-dryrun: fixture .app missing: $FIXTURE_APP" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Build the stub xcrun on PATH. It logs every invocation to $STUB_LOG and
# rejects any submit/staple/log subcommand. hdiutil is NOT stubbed — the
# real binary runs, so the DMG actually gets built (the script's contract).
# ---------------------------------------------------------------------------
TMP_DIR="$(mktemp -d -t notarize-dryrun-XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

STUB_BIN="$TMP_DIR/bin"
STUB_LOG="$TMP_DIR/xcrun.log"
WORK_DIR="$TMP_DIR/work"
mkdir -p "$STUB_BIN" "$WORK_DIR"

cat > "$STUB_BIN/xcrun" <<'STUB'
#!/usr/bin/env bash
# Recording shim. Logs ALL invocations, fails on submit/staple/log.
printf '%s\n' "xcrun $*" >> "$STUB_LOG"
for arg in "$@"; do
    case "$arg" in
        submit|staple|log)
            echo "STUB-XCRUN-REFUSE: dry-run must not invoke 'xcrun $arg' — saw: $*" >&2
            exit 99
            ;;
    esac
done
# Accept anything else (e.g. --version) and return a fake success.
echo "xcrun stub: $*"
exit 0
STUB
chmod +x "$STUB_BIN/xcrun"

# ---------------------------------------------------------------------------
# Invoke the script under test with the stub at the front of PATH. cd into
# WORK_DIR so the default build/ output is contained.
# ---------------------------------------------------------------------------
cd "$WORK_DIR"
RC=0
PATH="$STUB_BIN:$PATH" "$NOTARIZE" \
    --app "$FIXTURE_APP" \
    --version "0.0.0-test" \
    --dry-run \
    > "$TMP_DIR/run.stdout" 2> "$TMP_DIR/run.stderr" || RC=$?

if [[ "$RC" -ne 0 ]]; then
    echo "test-notarize-dryrun: notarize-mac.sh --dry-run exited $RC (expected 0)" >&2
    echo "--- stdout ---" >&2
    cat "$TMP_DIR/run.stdout" >&2
    echo "--- stderr ---" >&2
    cat "$TMP_DIR/run.stderr" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# NEGATIVE assertion (tests-that-pin-bugs.md): the stub's submit/staple/log
# refusal path must NEVER have fired. The stub exits 99 on submit/staple/log;
# the script propagates non-zero rc, so the rc check above already covers
# the "submit was called" case. We additionally grep the recording log to
# guarantee no invocation contained those subcommands.
# ---------------------------------------------------------------------------
if [[ -f "$STUB_LOG" ]] && grep -E '(^| )(submit|staple|log)( |$)' "$STUB_LOG" >/dev/null; then
    echo "test-notarize-dryrun: ASSERTION FAILED — dry-run invoked xcrun submit/staple/log" >&2
    echo "--- xcrun.log ---" >&2
    cat "$STUB_LOG" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Positive sanity: the DMG and the placeholder dry-run marker MUST exist
# (AC-14.2.1). This is a presence check, not a behavior assertion.
# ---------------------------------------------------------------------------
EXPECTED_DMG="$WORK_DIR/build/Human-0.0.0-test.dmg"
EXPECTED_MARKER="${EXPECTED_DMG}.dryrun"
if [[ ! -f "$EXPECTED_DMG" ]]; then
    echo "test-notarize-dryrun: expected DMG missing: $EXPECTED_DMG" >&2
    exit 3
fi
if [[ ! -f "$EXPECTED_MARKER" ]]; then
    echo "test-notarize-dryrun: expected dry-run marker missing: $EXPECTED_MARKER" >&2
    exit 3
fi

# Sanity: stdout must contain the dry-run banner so operators see what was
# skipped. (Negative assertion lives above; this is just observability.)
if ! grep -q 'DRY RUN: would call xcrun notarytool submit' "$TMP_DIR/run.stdout"; then
    echo "test-notarize-dryrun: dry-run did not print expected 'DRY RUN: would call xcrun notarytool submit' banner" >&2
    cat "$TMP_DIR/run.stdout" >&2
    exit 3
fi

echo "test-notarize-dryrun: OK — dry-run completed without invoking xcrun submit/staple/log"
exit 0
