#!/usr/bin/env bash
# lora-runner-ab.sh — End-to-end LoRA A/B evaluation orchestrator.
#
# Closes the canonical four-step LoRA fidelity workflow into a single
# command:
#
#   1. Run the persona's example bank through the BASE provider →
#      `before.json` (no adapter loaded).
#   2. Re-run the same prompts with the LoRA adapter loaded →
#      `after.json`.
#   3. Score both response sets against the persona's communication-
#      style fingerprint (synthetic or learned from
#      `personal_model.bin`) and report the delta.
#   4. Optionally enforce a CI gate via `--floor-delta` /
#      `--require-positive`.
#
# This is the operational counterpart to `scripts/check-lora-ab.sh`,
# which runs the comparator against pre-canned fixtures. THIS script
# generates the response sets live by calling a real provider, then
# invokes the same comparator.
#
# REQUIRED:
#   --persona <name>      Persona name (must exist in HU_PERSONA_DIR
#                         or ~/.human/personas/).
#   --adapter <path>      Path to the LoRA adapter to evaluate.
#
# OPTIONAL:
#   --output-dir <dir>    Where to write before.json/after.json/
#                         status.json. Defaults to a tempdir; set
#                         to a stable path to keep artifacts.
#   --provider <name>     Override the provider (default: from
#                         config; must support load_adapter — the
#                         huml provider does, cloud providers don't).
#   --model <id>          Provider model id (default: provider
#                         default).
#   --max-examples N      Cap responses per side.
#   --floor-delta F       CI gate: fail when delta < F.
#                         (default: no gate).
#   --require-positive    CI gate: fail when delta <= 0.
#   --keep                Don't delete the output dir on exit (only
#                         meaningful when --output-dir is the default
#                         tempdir).
#   --no-publish          Skip the post-success publish to
#                         $HUMAN_FIDELITY_AB_PATH (or
#                         ~/.human/last_fidelity_ab.json). Useful for
#                         test harnesses that want the run's
#                         status.json but must not touch the user's
#                         canonical state.
#
# DEPENDENCIES:
#   - `human` binary built from this repo (auto-built when missing).
#   - A provider configured to actually answer chat() calls. The
#     local huml provider works without API keys but requires
#     HU_ENABLE_LLAMACPP plus a downloaded GGUF; cloud providers
#     need an API key and don't support load_adapter.
#
# EXIT CODES:
#   0  — A/B run completed AND all gates passed.
#   1  — Bad arguments / setup failure.
#   2  — Either side of the runner returned errors for ALL examples
#        (no usable data).
#   3  — Comparator gate failed (delta below floor or non-positive
#        when --require-positive set).
#
# SAFETY:
#   The runner's --adapter handling fails the entire process if the
#   adapter can't load — this is intentional, since a base-model
#   "after.json" would silently zero the delta. The orchestrator
#   inherits that behavior: any single-step failure aborts the run
#   instead of producing misleading numbers.

set -euo pipefail

usage() {
    sed -n '2,/^set -euo pipefail/p' "$0" | sed 's/^# \{0,1\}//' | head -n 60
    exit "${1:-1}"
}

PERSONA=""
ADAPTER=""
OUTPUT_DIR=""
PROVIDER=""
MODEL=""
MAX_EXAMPLES=""
FLOOR_DELTA=""
REQUIRE_POSITIVE=""
KEEP=0
NO_PUBLISH=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --persona) PERSONA="$2"; shift 2 ;;
        --adapter) ADAPTER="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --provider) PROVIDER="$2"; shift 2 ;;
        --model) MODEL="$2"; shift 2 ;;
        --max-examples) MAX_EXAMPLES="$2"; shift 2 ;;
        --floor-delta) FLOOR_DELTA="$2"; shift 2 ;;
        --require-positive) REQUIRE_POSITIVE="--require-positive"; shift ;;
        --keep) KEEP=1; shift ;;
        --no-publish) NO_PUBLISH=1; shift ;;
        -h|--help) usage 0 ;;
        *) echo "[lora-runner-ab] unknown arg: $1" >&2; usage 1 ;;
    esac
done

[[ -z "$PERSONA" ]] && { echo "[lora-runner-ab] --persona is required" >&2; usage 1; }
[[ -z "$ADAPTER" ]] && { echo "[lora-runner-ab] --adapter is required" >&2; usage 1; }
[[ ! -f "$ADAPTER" ]] && { echo "[lora-runner-ab] adapter not found: $ADAPTER" >&2; exit 1; }

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HUMAN_BIN="${HUMAN_BIN:-$REPO_ROOT/build/human}"

# Auto-build only when the binary is missing — don't pay the rebuild
# cost on every invocation. CI typically pre-builds; local dev
# typically already has a build/ tree.
if [[ ! -x "$HUMAN_BIN" ]]; then
    echo "[lora-runner-ab] building $HUMAN_BIN..."
    cmake --build "$REPO_ROOT/build" --target human -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null
fi
[[ ! -x "$HUMAN_BIN" ]] && { echo "[lora-runner-ab] failed to build: $HUMAN_BIN" >&2; exit 1; }

# Stable vs ephemeral output dir. The keep-or-cleanup decision is made
# now (before any runs) so the EXIT trap doesn't touch a user-supplied
# directory it didn't create.
CREATED_TMP=0
if [[ -z "$OUTPUT_DIR" ]]; then
    OUTPUT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/lora-ab.XXXXXX")"
    CREATED_TMP=1
fi
mkdir -p "$OUTPUT_DIR"

cleanup() {
    if [[ "$CREATED_TMP" -eq 1 && "$KEEP" -eq 0 ]]; then
        rm -rf "$OUTPUT_DIR"
    fi
}
trap cleanup EXIT

BEFORE_JSON="$OUTPUT_DIR/before.json"
AFTER_JSON="$OUTPUT_DIR/after.json"
STATUS_JSON="$OUTPUT_DIR/status.json"

extra_args=()
[[ -n "$PROVIDER" ]] && extra_args+=(--provider "$PROVIDER")
[[ -n "$MODEL" ]] && extra_args+=(--model "$MODEL")
[[ -n "$MAX_EXAMPLES" ]] && extra_args+=(--max-examples "$MAX_EXAMPLES")

# Sanity: each runner invocation succeeds (exit 0) even when every
# example's chat call errored — it just writes a file of empty
# strings ["","",...]. That's a useful signal for the dashboard
# (visible 'no responses' state) but a poison pill for the
# comparator (delta against empty would be misleading). Catch it
# AFTER each side rather than after both, so a broken setup fails
# fast on step 1 instead of paying for step 2.
empty_response_set() {
    # True iff the file exists and contains no character outside the
    # empty-array boilerplate `[]" ,\n`. We strip those chars with
    # `tr -d` (portable across GNU + BSD; no regex bracket-class
    # escaping pitfalls) and check whether anything remains. The
    # original BRE form `[^"\[\] ,]` was BSD-grep-broken on macOS
    # (the parser saw the class as `[^"\[]` followed by literal
    # `\] ,]`, causing valid response arrays like `["ok"]` to be
    # falsely flagged as empty). Real escaped chars in responses
    # (e.g. `\n`, `\"`) leave a `\` in the residue and trigger the
    # non-empty path — same intent the original regex had, but
    # reliably this time.
    local f="$1"
    [[ -s "$f" ]] || return 0
    [[ -z "$(tr -d '[]" ,\n' < "$f")" ]]
}

echo "[lora-runner-ab] step 1/3: BASE responses → $BEFORE_JSON"
"$HUMAN_BIN" ml lora-runner --persona "$PERSONA" --output "$BEFORE_JSON" "${extra_args[@]}"
if empty_response_set "$BEFORE_JSON"; then
    echo "[lora-runner-ab] FAIL: $BEFORE_JSON has no non-empty responses — provider unreachable?" >&2
    exit 2
fi

echo "[lora-runner-ab] step 2/3: ADAPTER responses → $AFTER_JSON"
"$HUMAN_BIN" ml lora-runner --persona "$PERSONA" --output "$AFTER_JSON" \
    --adapter "$ADAPTER" "${extra_args[@]}"
if empty_response_set "$AFTER_JSON"; then
    echo "[lora-runner-ab] FAIL: $AFTER_JSON has no non-empty responses — adapter run failed silently?" >&2
    exit 2
fi

echo "[lora-runner-ab] step 3/3: comparing"
ab_args=(--persona "$PERSONA" --before "$BEFORE_JSON" --after "$AFTER_JSON")
[[ -n "$FLOOR_DELTA" ]] && ab_args+=(--floor-delta "$FLOOR_DELTA")
[[ -n "$REQUIRE_POSITIVE" ]] && ab_args+=("$REQUIRE_POSITIVE")

if ! "$HUMAN_BIN" ml lora-ab "${ab_args[@]}"; then
    echo "[lora-runner-ab] FAIL: comparator gate did not pass" >&2
    exit 3
fi

# Emit the structured fidelity-status JSON for any downstream
# dashboard / status surface (matches the hu-fidelity-tile
# component's data contract).
"$HUMAN_BIN" ml fidelity-status --persona "$PERSONA" \
    --before "$BEFORE_JSON" --after "$AFTER_JSON" \
    --output "$STATUS_JSON"
echo "[lora-runner-ab] PASS — status written to $STATUS_JSON"

# Publish status.json to the canonical path that
# `cp_admin_metrics_fidelity` reads. Atomic: write to a tmp file on
# the same filesystem as the destination (so `mv` is rename(2)), then
# rename. `set -e` already aborted on every prior failure path
# (exit 1=bad args, exit 2=empty response set, exit 3=gate fail), so
# reaching this line means the run produced a valid status.json.
if [[ "$NO_PUBLISH" -eq 0 ]]; then
    dest="${HUMAN_FIDELITY_AB_PATH:-${HOME:?HOME unset; pass --no-publish or set HUMAN_FIDELITY_AB_PATH}/.human/last_fidelity_ab.json}"
    mkdir -p "$(dirname "$dest")"
    tmpfile="$(mktemp "${dest}.XXXXXX")"
    cp "$STATUS_JSON" "$tmpfile"
    mv "$tmpfile" "$dest"
    echo "[lora-runner-ab] published → $dest"
fi

[[ "$KEEP" -eq 1 || "$CREATED_TMP" -eq 0 ]] && echo "  artifacts: $OUTPUT_DIR"
