#!/usr/bin/env bash
# Tests for scripts/install-local-vision.sh --dry-run: the rendered
# ai.human.vision-server plist is pinned to 127.0.0.1, offline, on 8746, and
# the installer refuses the prod ports and a template that drifts off loopback.
# Hermetic: --dry-run writes nothing; HOME is a temp dir; no launchctl.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; echo "$3" | sed 's/^/  /'; fail=1; fi; }
H=$(mktemp -d)

out=$(HOME="$H" bash "$HERE/install-local-vision.sh" --dry-run 2>&1); rc=$?
check "dry run succeeds" "[[ $rc -eq 0 ]]" "$out"
check "host is 127.0.0.1" "grep -A1 -- '<string>--host</string>' <<<\"\$out\" | grep -q '<string>127.0.0.1</string>'" "$out"
check "port 8746" "grep -A1 -- '<string>--port</string>' <<<\"\$out\" | grep -q '<string>8746</string>'" "$out"
check "HF_HUB_OFFLINE=1" "grep -A1 '<key>HF_HUB_OFFLINE</key>' <<<\"\$out\" | grep -q '<string>1</string>'" "$out"
check "no placeholder left" "! grep -q '@[A-Z]*@' <<<\"\$out\"" "$out"
check "dry run wrote nothing under HOME" "[[ -z \"\$(ls -A \"$H\")\" ]]" "$(ls -A "$H")"

for p in 8741 8743; do
    out=$(HOME="$H" HU_VISION_PORT=$p bash "$HERE/install-local-vision.sh" --dry-run 2>&1); rc=$?
    check "refuses prod port $p" "[[ $rc -ne 0 && \"\$out\" == *'belongs to the serving model'* ]]" "$out"
done

out=$(HOME="$H" HU_VISION_MODEL='x"; rm -rf /' bash "$HERE/install-local-vision.sh" --dry-run 2>&1); rc=$?
check "refuses a malformed model id" "[[ $rc -ne 0 ]]" "$out"

# A template edited to listen on every interface must be refused.
T=$(mktemp -d); mkdir -p "$T/scripts/launchd"
cp "$HERE/install-local-vision.sh" "$T/scripts/"
sed 's#<string>127.0.0.1</string>#<string>0.0.0.0</string>#' \
    "$HERE/launchd/ai.human.vision-server.plist.template" >"$T/scripts/launchd/ai.human.vision-server.plist.template"
out=$(HOME="$H" bash "$T/scripts/install-local-vision.sh" --dry-run 2>&1); rc=$?
check "refuses a template not pinned to loopback" "[[ $rc -ne 0 && \"\$out\" == *'not pinned to 127.0.0.1'* ]]" "$out"

rm -rf "$H" "$T"
[[ $fail -eq 0 ]] && echo "ALL PASS" || { echo "SOME FAILED"; exit 1; }
