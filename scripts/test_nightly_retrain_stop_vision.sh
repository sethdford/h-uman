#!/usr/bin/env bash
# Tests for stop_vision() / restore_vision() in nightly-retrain.sh — the
# optional local vision server (ai.human.vision-server, HU_LOCAL_VISION) is
# booted out for the training window and bootstrapped back afterwards.
#
# Hermetic: sources nightly-retrain.sh with HU_RETRAIN_STAGE_TEST=1 (functions
# only, returns before any real work), fakes HOME, and puts a fake launchctl
# FIRST on PATH. No real launchd job, process or ~/.human tree is touched.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$HERE/nightly-retrain.sh"
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; echo "$3" | sed 's/^/  /'; fail=1; fi; }

# make_fakes <dir> <installed 0|1> <print_rc> <bootout_rc>
make_fakes() {
    local dir="$1"
    mkdir -p "$dir/home/.human/logs" "$dir/home/Library/LaunchAgents"
    [[ "$2" == 1 ]] && echo '<plist/>' >"$dir/home/Library/LaunchAgents/ai.human.vision-server.plist"
    printf '#!/bin/sh\necho "$*" >> "%s/launchctl.args"\ncase "$1" in print) exit %s;; bootout) exit %s;; *) exit 0;; esac\n' \
        "$dir" "$3" "$4" >"$dir/launchctl"
    chmod +x "$dir/launchctl"
}

# run <fakedir>: stop_vision, then restore_vision; print both flag values.
run() {
    HOME="$1/home" PATH="$1:$PATH" HU_RETRAIN_STAGE_TEST=1 bash -c '
        source "'"$SCRIPT"'"
        stop_vision; rc=$?
        echo "rc=$rc after_stop=$vision_stopped"
        restore_vision
        echo "after_restore=$vision_stopped"
    ' 2>&1
}

UID_=$(id -u)
LABEL="gui/$UID_/ai.human.vision-server"

# 1. Not installed: launchctl is never asked anything.
F=$(mktemp -d); make_fakes "$F" 0 0 0; out=$(run "$F")
check "not installed: rc 0, nothing stopped" "[[ \"\$out\" == *'rc=0 after_stop=0'* ]]" "$out"
check "not installed: launchctl never called" "[[ ! -s \"$F/launchctl.args\" ]]" "$(cat "$F/launchctl.args" 2>/dev/null)"
rm -rf "$F"

# 2. Installed + loaded: bootout THE label, then bootstrap the plist back.
F=$(mktemp -d); make_fakes "$F" 1 0 0; out=$(run "$F"); args=$(cat "$F/launchctl.args")
check "loaded: stopped" "[[ \"\$out\" == *'rc=0 after_stop=1'* ]]" "$out"
check "loaded: bootout of exactly $LABEL" "grep -qx 'bootout $LABEL' <<<\"\$args\"" "$args"
check "loaded: bootstrap of the plist on restore" \
    "grep -qx 'bootstrap gui/$UID_ $F/home/Library/LaunchAgents/ai.human.vision-server.plist' <<<\"\$args\"" "$args"
check "loaded: flag cleared after restore" "[[ \"\$out\" == *'after_restore=0'* ]]" "$out"
rm -rf "$F"

# 3. Installed but not loaded (owner stopped it): left alone, never started.
F=$(mktemp -d); make_fakes "$F" 1 113 0; out=$(run "$F"); args=$(cat "$F/launchctl.args")
check "not loaded: nothing stopped" "[[ \"\$out\" == *'rc=0 after_stop=0'* ]]" "$out"
check "not loaded: no bootout, no bootstrap" "! grep -qE '^(bootout|bootstrap)' <<<\"\$args\"" "$args"
rm -rf "$F"

# 4. Bootout fails: training is not blocked and the trap does not bootstrap.
F=$(mktemp -d); make_fakes "$F" 1 0 3; out=$(run "$F"); args=$(cat "$F/launchctl.args")
check "bootout fails: rc 0, nothing marked stopped" "[[ \"\$out\" == *'rc=0 after_stop=0'* ]]" "$out"
check "bootout fails: warns" "[[ \"\$out\" == *'WARNING: launchctl bootout'* ]]" "$out"
check "bootout fails: no bootstrap" "! grep -q '^bootstrap' <<<\"\$args\"" "$args"
rm -rf "$F"

# 5. The main flow wires both: stop before serving stops, restore in the trap.
check "main flow calls stop_vision before stop_serving" \
    "grep -B1 -x 'stop_serving || exit 1' \"$SCRIPT\" | grep -qx 'stop_vision'" ""
check "EXIT trap restores vision" "grep -q \"^trap 'restore_serving; restore_vision' EXIT\" \"$SCRIPT\"" ""

[[ $fail -eq 0 ]] && echo "ALL PASS" || { echo "SOME FAILED"; exit 1; }
