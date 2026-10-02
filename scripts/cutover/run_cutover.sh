#!/usr/bin/env bash
# run_cutover.sh — the human-level cut-over kit: ONE command that replays the
# owner's real held-out turns through production's reply path once per arm,
# judges every arm against Seth's real replies with the local cross-family
# judge, scores the memory probes, and writes a PROMOTE/HOLD report per gate.
#
#   scripts/cutover/run_cutover.sh                    # the real run (the lead runs it)
#   scripts/cutover/run_cutover.sh --estimate-only    # gates, arms and runtime, then stop
#   scripts/cutover/run_cutover.sh --dry-run          # whole pipeline on fakes, no real data
#
# Arms (scripts/cutover/cutover_plan.py): A = every candidate gate off, B =
# every candidate gate live, B-no-<gate> = B with that gate off. Only gates
# whose env name is in the binary are used. Every arm starts from production's
# gate env (--plist), so A is production minus the new stack.
#
# Safety: generation goes to the replay endpoint (default :8741) ONLY through
# the replay harness (sandboxed HOME/state, loopback only, null channel, no
# sends), strictly sequential with --delay-ms between calls. The judge is the
# local Ollama Gemma (loopback, paced). Real data stays in the private run dir
# ~/blind_ab_run/cutover-<date>[...] (0700 dirs, 0600 files), never in the
# repo; nothing but counts and aggregates is printed. The kit never writes
# ~/.human/blind_ab_gate.json and never runs the PlistBuddy commands it prints.
#
# Decision rules: pre-registered in scripts/cutover/cutover_report.py's header.
# Runbook: docs/guides/cutover-kit.md.
set -euo pipefail

KIT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$KIT/../.." && pwd)"
HARNESS_DIR="${HU_CUTOVER_HARNESS_DIR:-$REPO/scripts/blind_ab}"
MEMORY_DIR="${HU_CUTOVER_MEMORY_DIR:-$REPO/scripts/datasets}"

NAME="cutover-$(date +%Y%m%d)"
RUN_ROOT="$HOME/blind_ab_run"
TURNS=60
PROBES=50
SINCE_DAYS=30
PER_CONTACT=3
ENDPOINT="http://127.0.0.1:8741/v1"
MODEL=""
TEMPERATURE=0
DELAY_MS=3000
HUMAN="$REPO/build/human"
PLIST="$HOME/Library/LaunchAgents/ai.human.service-loop.plist"
STATE_SRC="$HOME/.human"
JUDGE_ENDPOINT="http://127.0.0.1:11434/v1/chat/completions"
JUDGE_MODEL="gemma4-26b-mmap"
JUDGE_PACE=2
PROBES_SRC=""
LOCOMO="$HOME/.human/datasets/locomo/locomo10.json"
SKIP_MEMORY=0
MEMORY_ALL_ARMS=0
SANDBOX=auto
DRY_RUN=0
ESTIMATE_ONLY=0
CHAT_DB=""
MEMORY_DB=""
REPORT_EXTRA=()

usage() { sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; cat <<EOF

Options:
  --name NAME            run name (default $NAME); real data goes to RUN_ROOT/NAME
  --run-root DIR         default ~/blind_ab_run (must be outside the repo)
  --turns N              held-out turns (default $TURNS)
  --since-days N         turns from the last N days (default $SINCE_DAYS)
  --probes N             memory probes (default $PROBES)
  --probes-file F        a probes.jsonl from scripts/datasets/locomo_to_replay.py
                         (default: convert $LOCOMO with --per-category 10)
  --skip-memory          no memory probes (the report marks the guard NOT EVALUATED)
  --memory-all-arms      run the probes on every arm, not just A and B
  --endpoint URL         loopback replay endpoint (default $ENDPOINT)
  --model M              model name the endpoint expects (default: the config's)
  --delay-ms N           pause between replay calls (default $DELAY_MS; :8741 is live)
  --human PATH           a human binary with \`replay\` (default build/human)
  --plist PATH           production's gate env (default the service-loop plist)
  --judge-endpoint URL   loopback judge (default Ollama $JUDGE_ENDPOINT)
  --judge-model M        default $JUDGE_MODEL
  --estimate-only        print gates, arms and the runtime estimate, then exit
  --dry-run              fixtures + a fake loopback server + a stub binary (or
                         --human with a real one): the whole pipeline, no real data
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --name) NAME="$2"; shift 2 ;;
        --run-root) RUN_ROOT="$2"; shift 2 ;;
        --turns) TURNS="$2"; shift 2 ;;
        --since-days) SINCE_DAYS="$2"; shift 2 ;;
        --probes) PROBES="$2"; shift 2 ;;
        --probes-file) PROBES_SRC="$2"; shift 2 ;;
        --skip-memory) SKIP_MEMORY=1; shift ;;
        --memory-all-arms) MEMORY_ALL_ARMS=1; shift ;;
        --endpoint) ENDPOINT="$2"; shift 2 ;;
        --model) MODEL="$2"; shift 2 ;;
        --delay-ms) DELAY_MS="$2"; shift 2 ;;
        --human) HUMAN="$2"; HUMAN_SET=1; shift 2 ;;
        --plist) PLIST="$2"; shift 2 ;;
        --judge-endpoint) JUDGE_ENDPOINT="$2"; shift 2 ;;
        --judge-model) JUDGE_MODEL="$2"; shift 2 ;;
        --estimate-only) ESTIMATE_ONLY=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

say() { printf '[cutover] %s\n' "$*"; }
die() { printf '[cutover] refusing: %s\n' "$*" >&2; exit 2; }

for f in replay_export_turns.py replay_driver.py replay_feed.py make_rating_sheet.py \
         synthetic_judge.py; do
    [ -f "$HARNESS_DIR/$f" ] || die "no $HARNESS_DIR/$f (the replay harness, PR #594; set HU_CUTOVER_HARNESS_DIR)"
done

# ── dry run: everything fake ──────────────────────────────────────────
SERVER_PID=""
cleanup() { [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true; }
trap cleanup EXIT
if [ "$DRY_RUN" = 1 ]; then
    FIX="$(mktemp -d "${TMPDIR:-/tmp}/cutover-dryrun.XXXXXX")"
    chmod 700 "$FIX"
    python3 "$KIT/dryrun_fixtures.py" serve --port-file "$FIX/port" &
    SERVER_PID=$!
    for _ in $(seq 1 100); do [ -s "$FIX/port" ] && break; sleep 0.05; done
    [ -s "$FIX/port" ] || die "fake server did not start"
    PORT="$(cat "$FIX/port")"
    python3 "$KIT/dryrun_fixtures.py" build "$FIX" --port "$PORT"
    RUN_ROOT="$FIX/runs"; NAME="cutover-dryrun"
    TURNS=12; PROBES=10; DELAY_MS=0; JUDGE_PACE=0
    ENDPOINT="http://127.0.0.1:$PORT/v1"
    JUDGE_ENDPOINT="http://127.0.0.1:$PORT/v1/chat/completions"; JUDGE_MODEL="fake-judge"
    CHAT_DB="$FIX/chat.db"; MEMORY_DB="$FIX/state_src/memory.db"; STATE_SRC="$FIX/state_src"
    PROBES_SRC="$FIX/probes.jsonl"
    command -v plutil >/dev/null && PLIST="$FIX/ai.human.service-loop.plist" || PLIST=""
    if [ -z "${HUMAN_SET:-}" ]; then HUMAN="$KIT/dryrun_fake_human.py"; SANDBOX=off; fi
    export HU_BLIND_AB_SKIP_ADDRESSBOOK=1
    # The fixture is tiny: these overrides are flagged in the report as NOT pre-registered.
    REPORT_EXTRA=(--boot 200 --min-judged 3 --min-text 3)
    say "DRY RUN in $FIX (fake server :$PORT, binary $(basename "$HUMAN"))"
fi

case "$ENDPOINT" in http://127.0.0.1:*|http://localhost:*|http://\[::1\]:*) ;; *) die "--endpoint must be loopback" ;; esac
case "$JUDGE_ENDPOINT" in http://127.0.0.1:*|http://localhost:*|http://\[::1\]:*) ;; *) die "--judge-endpoint must be loopback" ;; esac
[ -x "$HUMAN" ] || die "no executable $HUMAN (cmake --preset dev && nice -n 10 cmake --build build --target human)"
"$HUMAN" replay --help >/dev/null 2>&1 || die "$HUMAN has no \`replay\` command (needs the replay harness, PR #594)"

# ── gates and arms ────────────────────────────────────────────────────
GATES="$(python3 "$KIT/cutover_plan.py" gates --binary "$HUMAN" | paste -sd, -)"
[ -n "$GATES" ] || die "no candidate humanness gate found in $HUMAN"
ABSENT="$(python3 - "$GATES" "$KIT" <<'PY'
import sys; sys.path.insert(0, sys.argv[2])
from cutover_plan import CANDIDATE_GATES
have = set(sys.argv[1].split(","))
print(",".join(g for g in CANDIDATE_GATES if g not in have))
PY
)"
ARM_SPECS=()
while IFS= read -r line; do ARM_SPECS+=("$line"); done < <(python3 "$KIT/cutover_plan.py" arms --gates "$GATES")
N_ARMS=${#ARM_SPECS[@]}
MEM_ARM_SPECS=()
if [ "$MEMORY_ALL_ARMS" = 1 ]; then
    MEM_ARM_SPECS=("${ARM_SPECS[@]}")
else
    while IFS= read -r line; do MEM_ARM_SPECS+=("$line"); done < <(python3 "$KIT/cutover_plan.py" arms --gates "$GATES" --only A,B)
fi
N_MEM_ARMS=${#MEM_ARM_SPECS[@]}
[ "$SKIP_MEMORY" = 1 ] && N_MEM_ARMS=0
say "gates in the binary: $GATES"
[ -n "$ABSENT" ] && say "not in the binary (skipped): $ABSENT"
say "arms ($N_ARMS): $(for s in "${ARM_SPECS[@]}"; do printf '%s ' "${s%%:*}"; done)"
python3 "$KIT/cutover_plan.py" estimate --turns "$TURNS" --probes "$PROBES" --arms "$N_ARMS" \
    --mem-arms "$N_MEM_ARMS" --delay-ms "$DELAY_MS" --judge-pace-s "$JUDGE_PACE" | sed 's/^/[cutover] /'
[ "$ESTIMATE_ONLY" = 1 ] && exit 0

# ── preflight ─────────────────────────────────────────────────────────
RUN="$RUN_ROOT/$NAME"
MEMRUN="$RUN_ROOT/$NAME-mem"
[ -e "$RUN" ] && die "$RUN already exists (pick --name)"
if [ "$DRY_RUN" = 0 ]; then
    curl -sf --max-time 5 "${JUDGE_ENDPOINT%/v1/chat/completions}/api/tags" | grep -q "\"$JUDGE_MODEL" \
        || die "judge model $JUDGE_MODEL is not served at $JUDGE_ENDPOINT (ollama list)"
    [ -f "$PLIST" ] || die "no $PLIST (production's gate env)"
fi
GATE_FILE="$HOME/.human/blind_ab_gate.json"
gate_sum() { [ -f "$GATE_FILE" ] && shasum "$GATE_FILE" | cut -d' ' -f1 || echo absent; }
GATE_BEFORE="$(gate_sum)"
umask 077
mkdir -p "$RUN_ROOT"; chmod 700 "$RUN_ROOT"

# ── 1. export held-out turns + memory probes ──────────────────────────
say "1/5 export: $TURNS held-out turns from the last $SINCE_DAYS days"
EXPORT=(python3 "$HARNESS_DIR/replay_export_turns.py" --name "$NAME" --run-root "$RUN_ROOT"
        --limit "$TURNS" --since-days "$SINCE_DAYS" --per-contact "$PER_CONTACT")
[ -n "$CHAT_DB" ] && EXPORT+=(--db "$CHAT_DB")
[ -n "$MEMORY_DB" ] && EXPORT+=(--memory-db "$MEMORY_DB")
"${EXPORT[@]}" | sed 's/^/[cutover]   /'
python3 "$HARNESS_DIR/replay_driver.py" snapshot --name "$NAME" --run-root "$RUN_ROOT" \
    --state-src "$STATE_SRC" | sed 's/^/[cutover]   /'

if [ "$SKIP_MEMORY" = 0 ]; then
    [ -f "$MEMORY_DIR/memory_probe_score.py" ] || die "no $MEMORY_DIR/memory_probe_score.py (PR #593; set HU_CUTOVER_MEMORY_DIR, or --skip-memory)"
    mkdir -p "$MEMRUN/src"; chmod 700 "$MEMRUN" "$MEMRUN/src"
    if [ -z "$PROBES_SRC" ]; then
        [ -f "$LOCOMO" ] || die "no $LOCOMO (bash scripts/datasets/fetch_locomo.sh), or pass --probes-file / --skip-memory"
        python3 "$MEMORY_DIR/locomo_to_replay.py" --input "$LOCOMO" --out-dir "$MEMRUN/src" \
            --per-category $(( (PROBES + 4) / 5 )) >/dev/null
        PROBES_SRC="$MEMRUN/src/probes.jsonl"
    fi
    python3 "$KIT/cutover_plan.py" prep-probes --src "$PROBES_SRC" --dst "$MEMRUN/turns.jsonl" \
        --n "$PROBES" | sed 's/^/[cutover]   /'
    mkdir -p "$MEMRUN/state"; chmod 700 "$MEMRUN/state"
    # copy-on-write clone on APFS (memory.db is hundreds of MB); plain copy elsewhere
    cp -cR "$RUN/state/base" "$MEMRUN/state/base" 2>/dev/null \
        || { rm -rf "$MEMRUN/state/base"; cp -R "$RUN/state/base" "$MEMRUN/state/base"; }
fi

# ── 2. replay every arm (sequential, paced) ───────────────────────────
DRIVE=(python3 "$HARNESS_DIR/replay_driver.py" run --run-root "$RUN_ROOT" --human "$HUMAN"
       --endpoint "$ENDPOINT" --temperature "$TEMPERATURE" --delay-ms "$DELAY_MS"
       --sandbox "$SANDBOX")
[ -n "$MODEL" ] && DRIVE+=(--model "$MODEL")
[ -n "$PLIST" ] && DRIVE+=(--base-env-plist "$PLIST")
arm_flags() { for s in "$@"; do printf -- '--arm\n%s\n' "$s"; done; }
TURN_ARGS=(); while IFS= read -r l; do TURN_ARGS+=("$l"); done < <(arm_flags "${ARM_SPECS[@]}")
say "2/5 replay: $TURNS turns x $N_ARMS arms (delay ${DELAY_MS} ms)"
RC=0; "${DRIVE[@]}" --name "$NAME" "${TURN_ARGS[@]}" | sed 's/^/[cutover]   /' || RC=$?
[ "$RC" = 0 ] || say "WARNING: an arm is INCOMPLETE; the report will HOLD every gate (R5)"
if [ "$SKIP_MEMORY" = 0 ]; then
    MEM_ARGS=(); while IFS= read -r l; do MEM_ARGS+=("$l"); done < <(arm_flags "${MEM_ARM_SPECS[@]}")
    say "   memory probes: $PROBES probes x $N_MEM_ARMS arms"
    "${DRIVE[@]}" --name "$NAME-mem" "${MEM_ARGS[@]}" | sed 's/^/[cutover]   /' || true
fi

# ── 3. blind sheets + per-arm stats vs Seth ───────────────────────────
say "3/5 feed: blind 2AFC sheets per arm"
python3 "$HARNESS_DIR/replay_feed.py" --name "$NAME" --run-root "$RUN_ROOT" --sheets \
    >/dev/null || say "WARNING: replay_feed refused (partial arms); judging what exists"

# ── 4. local cross-family judge, one arm at a time ────────────────────
say "4/5 judge: $JUDGE_MODEL at $JUDGE_ENDPOINT (pace ${JUDGE_PACE}s)"
for s in "${ARM_SPECS[@]}"; do
    arm="${s%%:*}"; d="$RUN/feed/$arm"
    [ -f "$d/rating_sheet.csv" ] || { say "   $arm: no sheet (no text-vs-text turns)"; continue; }
    python3 "$KIT/judge_local.py" "$d/rating_sheet.csv" --out "$d/judged.csv" \
        --harness-dir "$HARNESS_DIR" --endpoint "$JUDGE_ENDPOINT" --model "$JUDGE_MODEL" \
        --pace-s "$JUDGE_PACE" --temp 0 2>/dev/null | sed "s/^/[cutover]   $arm: /" \
        || say "   $arm: judge failed (its detection is unmeasured)"
    [ -f "$d/judged.csv" ] && chmod 600 "$d/judged.csv"
done

# ── 5. report ─────────────────────────────────────────────────────────
say "5/5 report"
REPORT=(python3 "$KIT/cutover_report.py" --run-dir "$RUN" --harness-dir "$HARNESS_DIR"
        --gates-absent "$ABSENT" --judge-model "$JUDGE_MODEL" "${REPORT_EXTRA[@]+"${REPORT_EXTRA[@]}"}")
[ -n "$PLIST" ] && REPORT+=(--plist "$PLIST")
[ "$SKIP_MEMORY" = 0 ] && REPORT+=(--mem-run-dir "$MEMRUN" --probes "$MEMRUN/turns.jsonl" --memory-dir "$MEMORY_DIR")
"${REPORT[@]}"

[ "$(gate_sum)" = "$GATE_BEFORE" ] || { echo "[cutover] ERROR: $GATE_FILE changed during the run" >&2; exit 3; }
say "done. Private run dir: $RUN (the PlistBuddy commands above were NOT executed)"
