#!/usr/bin/env bash
# Story B (Sprint 2a) — end-to-end test for scripts/lora-runner-ab.sh.
#
# Sprint 1's hermetic Story B driver only exercised the publish block
# in isolation; the BSD-grep regex bug in `empty_response_set()` was
# masked because that driver bypassed the runner with a pre-cooked
# response file. This test wires the orchestrator end-to-end against a
# deterministic mock `human` binary that mimics the four CLI calls the
# orchestrator makes (`ml lora-runner` ×2, `ml lora-ab`, `ml fidelity-status`).
#
# Verifies four scenarios:
#   1. happy: non-empty response sets → canonical AB JSON published atomically
#   2. empty before: orchestrator exits 2, canonical file NOT written
#   3. empty after:  orchestrator exits 2, canonical file NOT written
#   4. --no-publish: orchestrator exits 0, canonical file NOT written
#
# Run from anywhere; the script resolves $ROOT relative to its own location.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
ORCHESTRATOR="$ROOT/scripts/lora-runner-ab.sh"

[[ -x "$ORCHESTRATOR" ]] || { echo "missing $ORCHESTRATOR" >&2; exit 99; }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/sp2a-storyB.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0
ac_pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
ac_fail() { echo "  FAIL: $1" >&2;  FAIL=$((FAIL + 1)); }

# --- mock human binary -----------------------------------------------------
# Reads $HUMAN_TEST_SCENARIO ∈ {happy, empty_before, empty_after, no_publish}
# and the adapter flag to decide which payload to write. The orchestrator's
# `empty_response_set()` is exercised against the mock's payloads, so a
# regression to the BSD-grep bug would surface here.
SHIM="$TMP/human"
cat >"$SHIM" <<'BASH'
#!/usr/bin/env bash
set -euo pipefail
[[ "${1:-}" == "ml" ]] || { echo "mock-human: expected 'ml', got '$*'" >&2; exit 91; }
shift
sub="${1:-}"; shift || true
case "$sub" in
  lora-runner)
    out=""
    has_adapter=0
    while [[ $# -gt 0 ]]; do
      case "$1" in
        --output) out="$2"; shift 2 ;;
        --adapter) has_adapter=1; shift 2 ;;
        --persona|--provider|--model|--max-examples) shift 2 ;;
        *) shift ;;
      esac
    done
    [[ -n "$out" ]] || { echo "mock-human: lora-runner missing --output" >&2; exit 92; }
    side="before"; [[ "$has_adapter" -eq 1 ]] && side="after"
    case "${HUMAN_TEST_SCENARIO:-happy}_${side}" in
      happy_before)        printf '%s' '["hi","yo","sure"]' >"$out" ;;
      happy_after)         printf '%s' '["hello!","yeah","sure thing :)"]' >"$out" ;;
      empty_before_before) printf '%s' '[]' >"$out" ;;
      empty_before_after)  printf '%s' '["unused"]' >"$out" ;;
      empty_after_before)  printf '%s' '["x","y","z"]' >"$out" ;;
      empty_after_after)   printf '%s' '[]' >"$out" ;;
      no_publish_before)   printf '%s' '["a","b"]' >"$out" ;;
      no_publish_after)    printf '%s' '["A","B"]' >"$out" ;;
      *) printf '%s' '[]' >"$out" ;;
    esac
    ;;
  lora-ab)
    exit 0
    ;;
  fidelity-status)
    out=""
    while [[ $# -gt 0 ]]; do
      case "$1" in --output) out="$2"; shift 2 ;; *) shift ;; esac
    done
    [[ -n "$out" ]] || { echo "mock-human: fidelity-status missing --output" >&2; exit 93; }
    printf '%s\n' '{"persona":"test","baseline":{"mean":0.5},"candidate":{"mean":0.7},"ab":{"delta":0.2}}' >"$out"
    ;;
  *) echo "mock-human: unexpected ml subcommand: $sub" >&2; exit 94 ;;
esac
BASH
chmod +x "$SHIM"

ADAPTER="$TMP/fake.lora"
echo "fake-adapter-bytes" >"$ADAPTER"
FAKE_HOME="$TMP/home"
CANONICAL="$FAKE_HOME/.human/last_fidelity_ab.json"
mkdir -p "$FAKE_HOME"

run_orch() {
  local scenario="$1"
  local extra=("${@:2}")
  HUMAN_BIN="$SHIM" \
  HUMAN_FIDELITY_AB_PATH="$CANONICAL" \
  HUMAN_TEST_SCENARIO="$scenario" \
  bash "$ORCHESTRATOR" \
    --persona test \
    --adapter "$ADAPTER" \
    --output-dir "$TMP/run-$scenario" \
    --keep \
    "${extra[@]}"
}

# --- AC-B.1.a happy path ---------------------------------------------------
echo "== AC-B.1.a happy path =="
rm -f "$CANONICAL"
if run_orch happy >"$TMP/run-happy.log" 2>&1; then
  ac_pass "orchestrator exit 0 on non-empty responses"
else
  ac_fail "orchestrator non-zero on happy path (rc=$?)"
  cat "$TMP/run-happy.log" >&2
fi
if [[ -f "$CANONICAL" ]]; then ac_pass "canonical file written after happy run"; else ac_fail "canonical file missing"; fi
if [[ -f "$CANONICAL" ]] && grep -q '"delta":0.2' "$CANONICAL"; then
  ac_pass "canonical content has expected delta"
else
  ac_fail "canonical content missing expected delta payload"
fi

# Atomic-mv proof: the orchestrator writes via `mktemp ... && mv` so no
# partial canonical file should be left in $FAKE_HOME/.human/. There's
# no portable inotify available; instead, assert no leftover .XXXXXX
# tmp files in the canonical directory.
if compgen -G "${CANONICAL}.*" >/dev/null; then
  ac_fail "tmp file left next to canonical (atomic mv broken)"
else
  ac_pass "no leftover tmp file (atomic mv intact)"
fi

# --- AC-B.1.b empty before ------------------------------------------------
echo "== AC-B.1.b empty BEFORE short-circuits =="
rm -f "$CANONICAL"
set +e
run_orch empty_before >"$TMP/run-empty_before.log" 2>&1
rc=$?
set -e
if [[ "$rc" -eq 2 ]]; then ac_pass "exit 2 on empty before"; else ac_fail "expected exit 2, got $rc"; fi
if [[ ! -f "$CANONICAL" ]]; then ac_pass "canonical NOT written when before empty"; else ac_fail "canonical was written despite empty before"; fi

# --- AC-B.1.c empty after -------------------------------------------------
echo "== AC-B.1.c empty AFTER short-circuits =="
rm -f "$CANONICAL"
set +e
run_orch empty_after >"$TMP/run-empty_after.log" 2>&1
rc=$?
set -e
if [[ "$rc" -eq 2 ]]; then ac_pass "exit 2 on empty after"; else ac_fail "expected exit 2, got $rc"; fi
if [[ ! -f "$CANONICAL" ]]; then ac_pass "canonical NOT written when after empty"; else ac_fail "canonical was written despite empty after"; fi

# --- AC-B.1.d --no-publish honors flag ------------------------------------
echo "== AC-B.1.d --no-publish skips canonical write =="
rm -f "$CANONICAL"
if run_orch no_publish --no-publish >"$TMP/run-no_publish.log" 2>&1; then
  ac_pass "orchestrator exit 0 under --no-publish"
else
  ac_fail "orchestrator non-zero under --no-publish (rc=$?)"
  cat "$TMP/run-no_publish.log" >&2
fi
if [[ ! -f "$CANONICAL" ]]; then ac_pass "canonical NOT written under --no-publish"; else ac_fail "canonical written despite --no-publish"; fi

# --- regression sentinel: BSD-grep bug ------------------------------------
# If a future commit reverts empty_response_set() to the broken regex form,
# the happy path's `["hi","yo","sure"]` would be misclassified as empty
# and the orchestrator would exit 2. Re-run happy here as a sentinel.
echo "== sentinel: BSD-grep regex regression guard =="
rm -f "$CANONICAL"
if run_orch happy >"$TMP/run-sentinel.log" 2>&1 && [[ -f "$CANONICAL" ]]; then
  ac_pass "non-empty JSON arrays still classified as non-empty"
else
  ac_fail "regression: non-empty JSON misclassified — empty_response_set() may be broken"
  tail -20 "$TMP/run-sentinel.log" >&2 || true
fi

echo
echo "=========================================================="
echo "Story B (Sprint 2a) end-to-end: PASS=$PASS FAIL=$FAIL"
echo "=========================================================="
[[ "$FAIL" -eq 0 ]]
