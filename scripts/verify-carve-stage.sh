#!/usr/bin/env bash
# scripts/verify-carve-stage.sh — the evidence for one hu_agent_turn carve commit.
#
# Plan: docs/superpowers/plans/2026-09-30-agent-turn-carve-phase1.md. Run from
# the worktree root after a stage move (or on the unmodified tree for a
# baseline). Proves that the characterization goldens are untouched and pass,
# that the carve source pins and the full dev suite pass, and that the no-sqlite
# and minimal variants (both required CI jobs) compile. Prints every counter the
# PR body quotes. Stops at the first failure. Needs a configured build/
# (cmake --preset dev).
#
# "Untouched" is checked against git merge-base HEAD origin/main, never
# against the origin/main tip: a stage branch's own history diverges from
# origin/main as later commits land there, and diffing against the moving
# tip would blame this branch for changes it never made.
set -euo pipefail
root="$(git rev-parse --show-toplevel)"
cd "$root"
jobs="$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 8)"
logs="$(mktemp -d "${TMPDIR:-/tmp}/verify-carve.XXXXXX")"
trap 'rm -rf "$logs"' EXIT
step() { printf '\n== %s\n' "$*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

[ -f build/CMakeCache.txt ] || fail "no build/ — run: cmake --preset dev"

merge_base="$(git merge-base HEAD origin/main)"

step "intent-to-add new src/agent/turn/*.c and *.h files (clone ratchet only scans tracked files)"
git add -N src/agent/turn/*.c src/agent/turn/*.h 2>/dev/null || true
echo "ok"

step "goldens untouched vs merge-base with origin/main"
# --diff-filter=MD: Tasks 1-4 ADDED this fixture directory on this branch, so
# it does not exist at merge-base at all (PR #562 is not yet merged) — a
# plain diff would report every golden as "changed" forever. What this gate
# must catch is a LATER stage-move commit modifying or deleting an
# already-established golden, so it filters to Modified/Deleted only.
changed="$(git diff --name-only --diff-filter=MD "$merge_base" -- tests/fixtures/agent_turn_golden)"
[ -z "$changed" ] || fail "golden fixtures changed since merge-base (a stage commit may not regenerate them): $changed"
echo "ok"

step "dev build: human + human_tests"
cmake --build build --target human human_tests -j"$jobs" > "$logs/dev.log" 2>&1 \
    || { grep -E 'error:' "$logs/dev.log" | head -20; fail "dev build"; }
echo "ok"

step "characterization goldens"
./build/human_tests --suite=AgentTurnCharacterization > "$logs/char.log" 2>&1 \
    || { tail -60 "$logs/char.log"; fail "characterization"; }
# Require the LITERAL line the suite's PASS macro prints for this exact test
# ("  PASS  %s\n", tests/test_framework.h). A green "Results:" count alone is
# not enough: a suite-filter typo or an empty suite also prints a green
# Results line with zero of the tests that actually matter having run — and
# this is stronger than a bare `grep -q SKIP`, which false-positives on
# unrelated harness boilerplate the binary prints regardless of --suite
# (e.g. "SKIP  wasm WASI syscall tests (build with wasm32-wasi to run)").
# If build/ is not the configuration the goldens were made for and the test
# is SKIPped instead of run, this exact PASS line is absent either way, so
# one check covers both failure shapes.
grep -q 'PASS  characterization_matches_goldens' "$logs/char.log" \
    || { cat "$logs/char.log"; fail "characterization_matches_goldens did not report PASS"; }
grep '^--- Results:' "$logs/char.log"

if [ -f tests/test_turn_sources.c ]; then
    step "carve source pins"
    ./build/human_tests --suite=TurnSources > "$logs/src.log" 2>&1 \
        || { cat "$logs/src.log"; fail "TurnSources"; }
    grep -E 'spans|^--- Results:' "$logs/src.log"
fi

step "full dev suite"
./build/human_tests > "$logs/full.log" 2>&1 \
    || { grep -E 'FAIL|ERROR: AddressSanitizer' "$logs/full.log" | head -20; fail "full suite"; }
grep '^--- Results:' "$logs/full.log"

step "no-sqlite variant compiles (human + human_tests)"
if [ ! -f build-nosqlite/CMakeCache.txt ]; then
    cmake -S . -B build-nosqlite -DHU_ENABLE_SQLITE=OFF -DHU_ENABLE_ALL_CHANNELS=ON \
        > "$logs/nosqlite-cfg.log" 2>&1 || fail "configure build-nosqlite"
fi
cmake --build build-nosqlite --target human human_tests -j"$jobs" > "$logs/nosqlite.log" 2>&1 \
    || { grep -E 'error:' "$logs/nosqlite.log" | head -20; fail "no-sqlite build"; }
echo "ok"

step "minimal variant compiles"
if [ ! -f build-minimal/CMakeCache.txt ]; then
    cmake --preset minimal > "$logs/minimal-cfg.log" 2>&1 || fail "configure build-minimal"
fi
cmake --build build-minimal -j"$jobs" > "$logs/minimal.log" 2>&1 \
    || { grep -E 'error:' "$logs/minimal.log" | head -20; fail "minimal build"; }
echo "ok"

step "ratchets (quote in the PR body)"
turn_files="$(ls src/agent/turn/*.c 2>/dev/null || true)"
# shellcheck disable=SC2086
sh scripts/check-function-length-ceiling.sh src/agent/agent_turn.c src/daemon.c $turn_files 2>&1 | tail -4
bash scripts/check-file-size-ceiling.sh 2>&1 | tail -2
bash scripts/check-clone-ratchet.sh 2>&1 | grep -E 'Clone groups found|FAIL|NOTE' || true
bash scripts/check-sqlite-includer-ratchet.sh 2>&1 | tail -2
bash scripts/check-agent-flat-ratchet.sh 2>&1 | tail -2
bash scripts/check-agent-core-boundary.sh 2>&1 | tail -3
HU_DEAD_STRIP_STRICT=1 bash scripts/check-dead-strip-ratchet.sh 2>&1 \
    | grep -E '^A = |^B = |RATCHET_SKIP|FAIL' || true
bash scripts/check-test-source-gate-symmetry.sh 2>&1 | tail -2
bash tests/fixtures/check-agent-flat/run-smoke-test.sh

step "sizes"
wc -l src/agent/agent_turn.c $turn_files

echo
echo "verify-carve-stage: PASS"
