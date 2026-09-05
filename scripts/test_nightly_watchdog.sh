#!/usr/bin/env bash
# Tests for nightly-watchdog.sh: fake HOME, dry-run, artifact-based markers.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
T=$(mktemp -d); export HOME="$T"; mkdir -p "$T/.human/logs" "$T/.human/bin"
TODAY=$(date +%Y-%m-%d)
fail=0; check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }

# 1. nothing present -> all six missing
out=$(bash "$HERE/nightly-watchdog.sh" --dry-run)
check "all missing when no artifacts" "[[ \"$out\" == *'missing=[humanness doctor retrain drift authorship llm-judge eval logrotate semantic-gate]'* ]]"

# 2. today's humanness verdict present (non-empty) -> not missing
echo '{"composite":0.9}' > "$T/.human/logs/humanness-verdict-$TODAY.json"
out=$(bash "$HERE/nightly-watchdog.sh" --dry-run)
check "humanness verdict today satisfies marker" "[[ \"$out\" != *'humanness'* ]]"

# 3. an EMPTY verdict file does not count (artifact, not touch)
: > "$T/.human/logs/humanness-verdict-$TODAY.json"
out=$(bash "$HERE/nightly-watchdog.sh" --dry-run)
check "empty verdict file is not an artifact" "[[ \"$out\" == *'missing=[humanness'* ]]"

# 4. dated log line for doctor counts
echo "[${TODAY}T04:05:01] === nightly_eval done ===" > "$T/.human/logs/nightly-eval.log"
out=$(bash "$HERE/nightly-watchdog.sh" --dry-run)
check "dated nightly-eval line satisfies the eval marker" "[[ \"$out\" != *' eval'* ]]"
echo "[$TODAY] doctor fails=0" > "$T/.human/logs/doctor-nightly.log"
out=$(bash "$HERE/nightly-watchdog.sh" --dry-run)
check "dated doctor line satisfies marker" "[[ \"$out\" != *' doctor'* ]]"

# 5. missing wrapper is reported, never silently ignored
out=$(bash "$HERE/nightly-watchdog.sh" --dry-run)
check "retrain without wrapper is skipped:no-wrapper (dry keeps it in missing)" "[[ \"$out\" == *'retrain'* ]]"
# 6. retrain is refused outside 02-05 even when missing and its wrapper exists
mkdir -p "$T/repo/scripts"; printf '#!/bin/bash\necho "[%s] fake retrain" >> "$HOME/.human/logs/nightly-retrain.log"\n' "$(date +%Y-%m-%d)" > "$T/repo/scripts/nightly-retrain.sh"; chmod +x "$T/repo/scripts/nightly-retrain.sh"
out=$(HU_REPO_DIR="$T/repo" HU_WATCHDOG_HOUR=14 bash "$HERE/nightly-watchdog.sh" --dry-run)
check "retrain skipped outside its window at 14:00" "[[ \"$out\" == *'retrain:outside-window-02-05'* ]]"
out=$(HU_REPO_DIR="$T/repo" HU_WATCHDOG_HOUR=03 bash "$HERE/nightly-watchdog.sh" --dry-run)
check "retrain eligible inside its window at 03:00" "[[ \"$out\" == *'retrain(dry)'* ]]"
# 7. a REAL (non-dry) run executes a missing job under the mkdir lock and the
#    artifact it writes is what marks it done (flock does not exist on macOS)
printf '#!/bin/bash\necho "[%s] doctor fails=0" >> "$HOME/.human/logs/doctor-nightly.log"\n' "$(date +%Y-%m-%d)" > "$T/.human/bin/doctor-nightly.sh"; chmod +x "$T/.human/bin/doctor-nightly.sh"
rm -f "$T/.human/logs/doctor-nightly.log"
out=$(HU_REPO_DIR="$T/repo" HU_WATCHDOG_SKIP_HEALTH=1 HU_WATCHDOG_HOUR=14 bash "$HERE/nightly-watchdog.sh")
check "real run executes doctor and sees its artifact" "[[ \"$out\" == *'ran=[doctor]'* ]]"
check "lock dir released after the run" "[ ! -d \"$T/.human/locks/nightly.lock.d\" ]"
# rotate-logs.sh: an oversized log is copy-truncated (launchd keeps its O_APPEND fd valid),
# a small one is left alone, and the run leaves a dated marker line for the watchdog.
mkdir -p "$T/.human/logs"; head -c 3000000 /dev/zero | tr '\0' 'x' > "$T/.human/logs/service-loop-error.log"
echo "small" > "$T/.human/logs/tiny.log"
out=$(HU_ROTATE_MAX_BYTES=1000000 bash "$HERE/rotate-logs.sh")
check "oversized log truncated in place" "[ $(stat -f %z "$T/.human/logs/service-loop-error.log") -eq 0 ]"
check "rotated copy kept beside it" "ls "$T/.human/logs"/service-loop-error.log.*.gz >/dev/null 2>&1"
check "small log untouched" "[ \"$(cat "$T/.human/logs/tiny.log")\" = small ]"
check "rotation writes a dated marker" "grep -q \"^\\[$TODAY\" "$T/.human/logs/logrotate.log""
out=$(bash "$HERE/nightly-watchdog.sh" --dry-run)
check "watchdog sees the logrotate marker" "[[ \"$out\" != *'logrotate'* ]]"
# 8. semantic-gate is WEEKLY (lookback 7) and windowed 10-16: a record dated 5
#    days ago satisfies its marker, one dated 8 days ago does not; outside the
#    window it is skipped even when missing; the marker must be non-empty.
day_ago() { date -v-"$1"d +%Y-%m-%d 2>/dev/null || date -d "$1 days ago" +%Y-%m-%d; }
printf '#!/bin/bash\necho fake-gate\n' > "$T/repo/scripts/semantic_gate_weekly.sh"; chmod +x "$T/repo/scripts/semantic_gate_weekly.sh"
echo '{"verdict":"PROMOTE"}' > "$T/.human/logs/semantic-gate-$(day_ago 5).json"
out=$(HU_REPO_DIR="$T/repo" HU_WATCHDOG_HOUR=12 bash "$HERE/nightly-watchdog.sh" --dry-run)
check "semantic-gate record 5 days old satisfies the weekly marker" "[[ \"$out\" != *'semantic-gate'* ]]"
rm -f "$T/.human/logs/semantic-gate-$(day_ago 5).json"
echo '{"verdict":"PROMOTE"}' > "$T/.human/logs/semantic-gate-$(day_ago 8).json"
out=$(HU_REPO_DIR="$T/repo" HU_WATCHDOG_HOUR=12 bash "$HERE/nightly-watchdog.sh" --dry-run)
check "semantic-gate record 8 days old is stale -> eligible at 12:00" "[[ \"$out\" == *'semantic-gate(dry)'* ]]"
out=$(HU_REPO_DIR="$T/repo" HU_WATCHDOG_HOUR=03 bash "$HERE/nightly-watchdog.sh" --dry-run)
check "semantic-gate refused at 03:00 (retrain window)" "[[ \"$out\" == *'semantic-gate:outside-window-10-16'* ]]"
out=$(HU_REPO_DIR="$T/repo" HU_WATCHDOG_HOUR=04 bash "$HERE/nightly-watchdog.sh" --dry-run)
check "semantic-gate refused at 04:00 (nightly eval)" "[[ \"$out\" == *'semantic-gate:outside-window-10-16'* ]]"
: > "$T/.human/logs/semantic-gate-$TODAY.json"
out=$(HU_REPO_DIR="$T/repo" HU_WATCHDOG_HOUR=12 bash "$HERE/nightly-watchdog.sh" --dry-run)
check "empty semantic-gate file is not an artifact" "[[ \"$out\" == *'semantic-gate(dry)'* ]]"
# nightly jobs keep their 2-day lookback: a 5-day-old humanness verdict is missing
rm -f "$T/.human/logs/humanness-verdict-$TODAY.json"; echo '{"composite":0.9}' > "$T/.human/logs/humanness-verdict-$(day_ago 5).json"
out=$(HU_REPO_DIR="$T/repo" bash "$HERE/nightly-watchdog.sh" --dry-run)
check "5-day-old humanness verdict does NOT satisfy a nightly marker" "[[ \"$out\" == *'missing=[humanness'* ]]"
rm -rf "$T"; exit $fail
