#!/usr/bin/env bash
# Tests for stop_serving() in nightly-retrain.sh — the stage that boots the
# mlx-server job out of launchd before training.
#
# Hermetic: sources nightly-retrain.sh with HU_RETRAIN_STAGE_TEST=1 (defines
# log() / stop_serving() / run_mlxtune_candidate_stage() and returns before
# the window check or any real work), fakes HOME, and puts fake launchctl /
# pgrep / lsof / sleep / vm_stat executables FIRST on PATH. No real launchd
# job, process table, or ~/.human tree is touched.
#
# Why this exists: 2026-09-04 the real run logged "Boot-out failed: 3: No such
# process" then "FATAL: mlx-server still alive after bootout". The bootout
# targeted gui/501/gui/501/ai.human.mlx-server (domain doubled), so the server
# never stopped and training was refused every night since aa2a1a79b. Case 1
# pins the exact label; cases 2-4 pin the refuse/restore bookkeeping.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$HERE/nightly-retrain.sh"
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; echo "  --- output ---"; echo "$3" | sed 's/^/  /'; fail=1; fi; }

bash -n "$SCRIPT" || { echo "FAIL bash -n $SCRIPT"; exit 1; }
echo "PASS bash -n $SCRIPT"

# make_fakes <dir> <launchctl_rc> <alive_rc>
#   launchctl: appends its argv to <dir>/launchctl.args, exits <launchctl_rc>
#   pgrep/lsof: exit <alive_rc> (0 = the server process / listener exists)
#   sleep:     no-op, so the wait loop costs nothing
#   vm_stat:   a fixed "Pages free" line (6553600 pages = 100 GB)
make_fakes() {
    local dir="$1" lrc="$2" arc="$3"
    mkdir -p "$dir/home"
    printf '#!/bin/sh\necho "$*" >> "%s/launchctl.args"\n[ %s -eq 0 ] || echo "Boot-out failed: %s: No such process" >&2\nexit %s\n' "$dir" "$lrc" "$lrc" "$lrc" > "$dir/launchctl"
    printf '#!/bin/sh\nexit %s\n' "$arc" > "$dir/pgrep"
    printf '#!/bin/sh\nexit %s\n' "$arc" > "$dir/lsof"
    printf '#!/bin/sh\nexit 0\n' > "$dir/sleep"
    printf '#!/bin/sh\necho "Pages free:                             6553600."\n' > "$dir/vm_stat"
    chmod +x "$dir/launchctl" "$dir/pgrep" "$dir/lsof" "$dir/sleep" "$dir/vm_stat"
}

# run_stop <fakedir>: source the script, run stop_serving, and end the output
# with "rc=<n> serving_stopped=<n>".
run_stop() {
    local fakes="$1"
    HOME="$fakes/home" PATH="$fakes:$PATH" HU_RETRAIN_STAGE_TEST=1 HU_RETRAIN_STOP_WAIT_SECS=10 bash -c '
        source "'"$SCRIPT"'"
        serving_stopped=0
        stop_serving; rc=$?
        echo "rc=$rc serving_stopped=$serving_stopped"
    ' 2>&1
}

EXPECTED_LABEL="gui/$(id -u)/ai.human.mlx-server"

# ── Case 1: bootout succeeds, port frees — the happy path, and THE label ────
F1=$(mktemp -d); make_fakes "$F1" 0 1
out1=$(run_stop "$F1")
args1=$(cat "$F1/launchctl.args" 2>/dev/null)
check "happy: launchctl called exactly once, as 'bootout <label>'" \
    "[[ \"\$args1\" == \"bootout $EXPECTED_LABEL\" ]]" "$args1"
check "happy: the label carries the gui/<uid>/ domain exactly once (2026-09-04 regression)" \
    "[[ \"\$args1\" != *'gui/'*'gui/'* ]]" "$args1"
check "happy: returns 0 with serving_stopped=1" \
    "[[ \"\$out1\" == *'rc=0 serving_stopped=1'* ]]" "$out1"
check "happy: logs the freed memory" \
    "[[ \"\$out1\" == *'serving stopped; 100 GB free'* ]]" "$out1"
check "happy: no WARNING and no FATAL" \
    "[[ \"\$out1\" != *'WARNING'* && \"\$out1\" != *'FATAL'* ]]" "$out1"
rm -rf "$F1"

# ── Case 2: bootout fails AND the server is still alive — refuse, leave it ──
#    (the exact shape of the 2026-09-04 03:07 run)
F2=$(mktemp -d); make_fakes "$F2" 3 0
out2=$(run_stop "$F2")
check "unmanaged+alive: logs launchctl's own error" \
    "[[ \"\$out2\" == *'Boot-out failed: 3: No such process'* ]]" "$out2"
check "unmanaged+alive: warns that the bootout failed" \
    "[[ \"\$out2\" == *'WARNING: launchctl bootout '\"$EXPECTED_LABEL\"' failed (rc=3)'* ]]" "$out2"
check "unmanaged+alive: refuses with FATAL" \
    "[[ \"\$out2\" == *'FATAL: mlx-server still alive after bootout'* ]]" "$out2"
check "unmanaged+alive: returns 1 and leaves serving_stopped=0 (trap must NOT kickstart a server we never stopped)" \
    "[[ \"\$out2\" == *'rc=1 serving_stopped=0'* ]]" "$out2"
rm -rf "$F2"

# ── Case 3: bootout succeeds but the process lingers past the wait ─────────
F3=$(mktemp -d); make_fakes "$F3" 0 0
out3=$(run_stop "$F3")
check "booted-out+lingering: refuses with FATAL" \
    "[[ \"\$out3\" == *'FATAL: mlx-server still alive after bootout'* ]]" "$out3"
check "booted-out+lingering: returns 1 but serving_stopped=1 (we unloaded the job; trap must restore it)" \
    "[[ \"\$out3\" == *'rc=1 serving_stopped=1'* ]]" "$out3"
rm -rf "$F3"

# ── Case 4: bootout fails but nothing is serving — train, and restore after ─
F4=$(mktemp -d); make_fakes "$F4" 3 1
out4=$(run_stop "$F4")
check "unmanaged+dead: warns about the failed bootout" \
    "[[ \"\$out4\" == *'WARNING: launchctl bootout'* ]]" "$out4"
check "unmanaged+dead: proceeds (rc=0) with serving_stopped=1 so the trap brings serving back" \
    "[[ \"\$out4\" == *'rc=0 serving_stopped=1'* ]]" "$out4"
rm -rf "$F4"

if [[ "$fail" == "0" ]]; then echo "ALL PASS"; else echo "SOME FAILED"; fi
exit "$fail"
