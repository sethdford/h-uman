#!/usr/bin/env bash
# tests/fixtures/check-agent-flat/run-smoke-test.sh — smoke test for
# scripts/check-agent-flat-ratchet.sh. Spec 2026-09-30-agent-turn-carve §4.5 is
# met by that gate; this pins the two properties the hu_agent_turn carve relies
# on: (1) .c files in a sub-package (src/agent/turn/) are NOT counted, and
# (2) one flat src/agent/*.c over the ceiling fails.
#
# Runs the real script, with its baseline rewritten to 2, inside a throwaway git
# repo, so the real tree and its baseline constant are never touched.
# Run from the repo root: bash tests/fixtures/check-agent-flat/run-smoke-test.sh
set -euo pipefail
root="$(git rev-parse --show-toplevel)"
t="$(mktemp -d "${TMPDIR:-/tmp}/agent-flat-smoke.XXXXXX")"
trap 'rm -rf "$t"' EXIT
git -C "$t" init -q
mkdir -p "$t/scripts" "$t/src/agent/turn"
sed 's/^AGENT_FLAT_BASELINE=[0-9]*.*/AGENT_FLAT_BASELINE=2/' \
    "$root/scripts/check-agent-flat-ratchet.sh" > "$t/scripts/check-agent-flat-ratchet.sh"
grep -q '^AGENT_FLAT_BASELINE=2$' "$t/scripts/check-agent-flat-ratchet.sh" \
    || { echo "FAIL: could not rewrite AGENT_FLAT_BASELINE"; exit 1; }
touch "$t/src/agent/a.c" "$t/src/agent/b.c" \
      "$t/src/agent/turn/turn_x.c" "$t/src/agent/turn/turn_y.c" "$t/src/agent/turn/turn_z.c"

out="$(cd "$t" && HU_RATCHET_NO_AUTOLOCK=1 bash scripts/check-agent-flat-ratchet.sh 2>&1)" \
    || { echo "FAIL: sub-package files were counted: $out"; exit 1; }
case "$out" in
*"flat src/agent/*.c: 2 (ceiling 2)"*) ;;
*) echo "FAIL: unexpected output: $out"; exit 1 ;;
esac

touch "$t/src/agent/c.c"
if (cd "$t" && HU_RATCHET_NO_AUTOLOCK=1 bash scripts/check-agent-flat-ratchet.sh > /dev/null 2>&1); then
    echo "FAIL: a third flat src/agent/*.c did not fail the gate"
    exit 1
fi
echo "agent-flat smoke: PASS"
