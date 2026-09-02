#!/usr/bin/env bash
# Tests for nightly-watchdog.sh: fake HOME, dry-run, artifact-based markers.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
T=$(mktemp -d); export HOME="$T"; mkdir -p "$T/.human/logs" "$T/.human/bin"
TODAY=$(date +%Y-%m-%d)
fail=0; check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }

# 1. nothing present -> all three missing
out=$(bash "$HERE/nightly-watchdog.sh" --dry-run)
check "all missing when no artifacts" "[[ \"$out\" == *'missing=[humanness doctor retrain]'* ]]"

# 2. today's humanness verdict present (non-empty) -> not missing
echo '{"composite":0.9}' > "$T/.human/logs/humanness-verdict-$TODAY.json"
out=$(bash "$HERE/nightly-watchdog.sh" --dry-run)
check "humanness verdict today satisfies marker" "[[ \"$out\" != *'humanness'* ]]"

# 3. an EMPTY verdict file does not count (artifact, not touch)
: > "$T/.human/logs/humanness-verdict-$TODAY.json"
out=$(bash "$HERE/nightly-watchdog.sh" --dry-run)
check "empty verdict file is not an artifact" "[[ \"$out\" == *'missing=[humanness'* ]]"

# 4. dated log line for doctor counts
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
rm -rf "$T"; exit $fail
