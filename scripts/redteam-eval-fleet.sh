#!/usr/bin/env bash
# Red-team + evaluation fleet: offline C tests, eval suite inventory, optional live API runs.
#
# Default (no API spend): C tests (Eval* + Adversarial*), human doctor, human eval list,
#   adversarial-eval-harness dry-run with adversarial + capability_edges suites.
#
# Live (network + provider keys): set REDTEAM_FLEET_LIVE=1
#   - human eval run on a configurable list of JSON suites (reports under REDTEAM_REPORT_DIR)
#   - adversarial-eval-harness.py with synthetic probes + suite (needs ADV_EVAL_API_KEY)
#
# If .env points ADV_EVAL_* at a Gemini OpenAI-compat base URL but your key is OpenAI, set:
#   REDTEAM_HARNESS_USE_OPENAI=1   # ADV_EVAL_* -> api.openai.com + gpt-4o-mini (override with REDTEAM_ADV_EVAL_MODEL)
#
# Agent vs judge (decoupled):
#   REDTEAM_HARNESS_USE_OPENAI=1   # judge only: sets ADV_EVAL_* to OpenAI (needs OPENAI_API_KEY). Does NOT change HUMAN_*.
#   REDTEAM_AGENT_USE_OPENAI=1     # agent + eval: sets HUMAN_PROVIDER=openai and harness --agent-* (needs OPENAI_API_KEY)
#   REDTEAM_AGENT_USE_GEMINI=1     # agent + eval: sets HUMAN_PROVIDER=gemini (needs GEMINI_API_KEY); optional explicit pin
#   REDTEAM_AGENT_MODEL=my-model   # optional when using *_AGENT_USE_*; else HUMAN_MODEL or provider default
#
# Typical Gemini-vs-OpenAI comparison: keep HUMAN_PROVIDER/HUMAN_MODEL for the agent in .env, set
# REDTEAM_HARNESS_USE_OPENAI=1 for a comparable OpenAI judge, and do not set REDTEAM_AGENT_USE_OPENAI.
#
# Optional: ADV_EVAL_TIMEOUT_SEC=240 — longer OpenAI-compat HTTP timeout for harness judge/generator (default 180; retries 3×).
# Optional: HARNESS_AGENT_CONFIG_PATH=/path/config.json — harness passes HUMAN_CONFIG_PATH to each `human agent` child.
# Optional: REDTEAM_FLEET_AGENT_SMOKE=1 runs one human agent -m (uses HUMAN_PROVIDER / keys in env).
# Live harness: REDTEAM_PROBE_PROFILE=safety|capability_honesty|human_likeness|tool_capability for generated probes.
# Live harness: REDTEAM_AGENT_TIMEOUT=300 (default) seconds per `human agent -m` subprocess (raise if slow provider).
#
# Examples:
#   bash scripts/redteam-eval-fleet.sh
#   REDTEAM_FLEET_LIVE=1 set -a && source .env && set +a && bash scripts/redteam-eval-fleet.sh
#   bash scripts/redteam-live.sh   # sources .env, sets LIVE + OpenAI alignment + runs this script
#   REDTEAM_FLEET_LIVE=1 REDTEAM_PROBES=5 REDTEAM_EVAL_SUITES="eval_suites/adversarial.json" bash scripts/redteam-eval-fleet.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

EXIT_CODE=0

REPORT_DIR="${REDTEAM_REPORT_DIR:-$ROOT/build/redteam-fleet-reports}"
HUMAN_BIN="${HUMAN_BIN:-$ROOT/build/human}"
HARNESS="$ROOT/scripts/adversarial-eval-harness.py"
# Space-separated paths, relative to ROOT
DEFAULT_EVAL_SUITES="eval_suites/reasoning_basic.json eval_suites/tool_use_basic.json eval_suites/adversarial.json eval_suites/capability_edges.json eval_suites/human_likeness.json eval_suites/tool_capability.json eval_suites/hula_orchestration.json eval_suites/coding_basic.json eval_suites/multi_turn.json"

run() {
  local name="$1"
  shift
  echo ""
  echo "=== redteam-eval-fleet: $name ==="
  if "$@"; then
    echo "--- OK: $name"
  else
    echo "--- FAIL: $name"
    EXIT_CODE=1
  fi
}

skip() {
  local name="$1"
  local why="$2"
  echo ""
  echo "=== redteam-eval-fleet: $name ==="
  echo "--- SKIP: $why"
}

echo "=============================="
echo " redteam-eval-fleet"
echo "  LIVE=${REDTEAM_FLEET_LIVE:-0} AGENT_SMOKE=${REDTEAM_FLEET_AGENT_SMOKE:-0}"
echo "=============================="

if [ ! -x "$HUMAN_BIN" ]; then
  echo "redteam-eval-fleet: no executable at HUMAN_BIN=$HUMAN_BIN (build the project first)" >&2
  exit 1
fi

if [ ! -f "$HARNESS" ]; then
  echo "redteam-eval-fleet: missing $HARNESS" >&2
  exit 1
fi

# --- Offline: C tests (substring suite filters — avoid --suite=Eval; matches "retrieval") ---
if [ -f "$ROOT/build/human_tests" ]; then
  run "C tests (suite Adversarial)" "$ROOT/build/human_tests" --suite=Adversarial
  run "C tests (suite: Evaluation Harness)" "$ROOT/build/human_tests" --suite="Evaluation Harness"
  run "C tests (suite: Eval Judge)" "$ROOT/build/human_tests" --suite="Eval Judge"
  run "C tests (suite: Eval History Storage)" "$ROOT/build/human_tests" --suite="Eval History"
  run "C tests (suite: eval_runner)" "$ROOT/build/human_tests" --suite=eval_runner
  run "C tests (suite: eval_bench)" "$ROOT/build/human_tests" --suite=eval_bench
  run "C tests (suite: Weakness Analyzer)" "$ROOT/build/human_tests" --suite="Weakness Analyzer"
else
  skip "C tests" "build/human_tests not found"
  EXIT_CODE=1
fi

# `human doctor` is a DIAGNOSTIC: it intentionally exits 1 whenever the install
# isn't fully configured (no ~/.human config_dir, no paired channel, no running
# daemon, build flags off). A fresh CI runner is never configured, so doctor
# always exits 1 there — gating the offline fleet on doctor exit-0 made this
# check permanently red in CI. The real smoke-test intent is "doctor runs to
# completion without crashing": tolerate the diagnostic exit (0=clean, 1=issues
# found) and fail only on a crash (>1, e.g. SIGSEGV=139 / SIGABRT=134). Output
# stays visible for triage. For a strict environment check, run live locally.
run "human doctor (smoke: runs without crashing; diagnostic exit tolerated)" \
  sh -c '"$0" doctor; rc=$?; [ "$rc" -le 1 ]' "$HUMAN_BIN"

run "human eval list" "$HUMAN_BIN" eval list

# --- Harness dry-run (no API keys): safety + epistemic / anti–AGI-overclaim probes ---
run "adversarial harness (dry-run, adversarial + capability + human + tool + hula + multi-turn)" \
  python3 "$HARNESS" --dry-run --no-llm \
  --include-suite "$ROOT/eval_suites/adversarial.json" \
  --include-suite "$ROOT/eval_suites/capability_edges.json" \
  --include-suite "$ROOT/eval_suites/human_likeness.json" \
  --include-suite "$ROOT/eval_suites/tool_capability.json" \
  --include-suite "$ROOT/eval_suites/hula_orchestration.json" \
  --include-suite "$ROOT/eval_suites/multi_turn.json"

# --- Live: human eval run ---
if [ "${REDTEAM_FLEET_LIVE:-0}" = 1 ]; then
  mkdir -p "$REPORT_DIR"
  TS="$(date -u +%Y%m%dT%H%M%SZ)"
  LIVE_DIR="$REPORT_DIR/live-$TS"
  mkdir -p "$LIVE_DIR"
  echo ""
  echo "=== redteam-eval-fleet: live eval runs -> $LIVE_DIR ==="

  if [ "${REDTEAM_HARNESS_USE_OPENAI:-0}" = 1 ] && [ -n "${OPENAI_API_KEY:-}" ]; then
    export ADV_EVAL_API_KEY="$OPENAI_API_KEY"
    export ADV_EVAL_BASE_URL="https://api.openai.com/v1"
    export ADV_EVAL_MODEL="${REDTEAM_ADV_EVAL_MODEL:-gpt-4o-mini}"
  fi

  HARNESS_PY_ARGS=()
  if [ "${REDTEAM_AGENT_USE_OPENAI:-0}" = 1 ] && [ -n "${OPENAI_API_KEY:-}" ]; then
    export HUMAN_PROVIDER="openai"
    export HUMAN_MODEL="${REDTEAM_AGENT_MODEL:-${HUMAN_MODEL:-gpt-4o-mini}}"
    HARNESS_PY_ARGS=(--agent-provider openai --agent-model "${HUMAN_MODEL}")
    echo "redteam-eval-fleet: agent/eval forced HUMAN_PROVIDER=openai HUMAN_MODEL=${HUMAN_MODEL}"
  elif [ "${REDTEAM_AGENT_USE_GEMINI:-0}" = 1 ] && [ -n "${GEMINI_API_KEY:-}" ]; then
    export HUMAN_PROVIDER="gemini"
    export HUMAN_MODEL="${REDTEAM_AGENT_MODEL:-${HUMAN_MODEL:-gemini-2.0-flash}}"
    HARNESS_PY_ARGS=(--agent-provider gemini --agent-model "${HUMAN_MODEL}")
    echo "redteam-eval-fleet: agent/eval forced HUMAN_PROVIDER=gemini HUMAN_MODEL=${HUMAN_MODEL}"
  else
    echo "redteam-eval-fleet: agent/eval using HUMAN_PROVIDER=${HUMAN_PROVIDER:-unset} HUMAN_MODEL=${HUMAN_MODEL:-unset} (from environment; harness inherits)"
  fi

  # Isolated HOME so eval runs don't load stale ~/.human/ memory/preferences
  EVAL_HOME="$(mktemp -d "${TMPDIR:-/tmp}/hu_eval_home_XXXXXX")"
  echo "redteam-eval-fleet: isolated HOME=$EVAL_HOME"

  SUITES="${REDTEAM_EVAL_SUITES:-$DEFAULT_EVAL_SUITES}"
  local_fail=0
  for suite in $SUITES; do
    path="$ROOT/${suite#./}"
    base="$(basename "$path" .json)"
    out="$LIVE_DIR/eval-${base}.log"
    echo "  running: $suite"
    if HOME="$EVAL_HOME" "$HUMAN_BIN" eval run "$path" >"$out" 2>&1; then
      echo "  --- OK: $suite"
    else
      echo "  --- FAIL: $suite (see $out)"
      local_fail=1
    fi
  done
  if [ "$local_fail" -ne 0 ]; then
    EXIT_CODE=1
  fi

  if [ -n "${ADV_EVAL_API_KEY:-}" ]; then
    probes="${REDTEAM_PROBES:-4}"
    probe_prof="${REDTEAM_PROBE_PROFILE:-safety}"
    echo ""
    echo "=== redteam-eval-fleet: dynamic harness (probes=$probes profile=$probe_prof) -> $LIVE_DIR/harness-report.json ==="
    # Empty HARNESS_PY_ARGS + set -u: "${arr[@]}" is unbound on some Bash versions; use ${arr[@]+"${arr[@]}"}
    if python3 "$HARNESS" ${HARNESS_PY_ARGS[@]+"${HARNESS_PY_ARGS[@]}"} --probes "$probes" --probe-profile "$probe_prof" \
      --include-suite "$ROOT/eval_suites/adversarial.json" \
      --include-suite "$ROOT/eval_suites/capability_edges.json" \
      --include-suite "$ROOT/eval_suites/human_likeness.json" \
      --include-suite "$ROOT/eval_suites/tool_capability.json" \
      --include-suite "$ROOT/eval_suites/hula_orchestration.json" \
      --include-suite "$ROOT/eval_suites/multi_turn.json" \
      --timeout "${REDTEAM_AGENT_TIMEOUT:-300}" \
      --output "$LIVE_DIR/harness-report.json" 2>&1 | tee "$LIVE_DIR/harness-console.log"; then
      echo "--- OK: dynamic harness"
    else
      echo "--- FAIL: dynamic harness"
      EXIT_CODE=1
    fi
  else
    skip "dynamic harness (live)" "ADV_EVAL_API_KEY unset"
  fi
else
  skip "human eval run (live)" "REDTEAM_FLEET_LIVE not set to 1"
  skip "dynamic harness (live)" "REDTEAM_FLEET_LIVE not set to 1"
fi

# --- Optional agent smoke ---
if [ "${REDTEAM_FLEET_AGENT_SMOKE:-0}" = 1 ]; then
  msg="${REDTEAM_AGENT_MESSAGE:-Reply with exactly: pong}"
  run "human agent smoke (-m)" "$HUMAN_BIN" agent -m "$msg"
else
  skip "human agent smoke" "REDTEAM_FLEET_AGENT_SMOKE not set to 1"
fi

echo ""
echo "=============================="
if [ "$EXIT_CODE" -eq 0 ]; then
  echo " redteam-eval-fleet: completed (exit 0)"
else
  echo " redteam-eval-fleet: completed with failures (exit $EXIT_CODE)"
fi
echo "=============================="

# Clean up isolated eval HOME
if [ -n "${EVAL_HOME:-}" ] && [ -d "$EVAL_HOME" ]; then
  rm -rf "$EVAL_HOME"
fi

exit "$EXIT_CODE"
