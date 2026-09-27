#!/usr/bin/env bash
# Wait until the simulator named in an xcodebuild -destination is available, then
# boot it, so `xcodebuild test` never resolves the destination against a
# CoreSimulator that has not loaded its runtimes yet.
#
# Why: on 2026-09-27 the iphone-16 fleet job failed on three runners in a row with
# "Unable to find a device matching the provided destination specifier", listing
# NO simulators at all (only the placeholder destinations), while the other two
# jobs on the same image found theirs. A missing device would still list the
# others; an empty list means the simulator service was not ready.
#
# Usage: wait-for-simulator.sh "platform=iOS Simulator,name=iPhone 16,OS=latest"
# Env:   SIM_WAIT_SECONDS (default 300), SIM_POLL_SECONDS (default 10)
set -euo pipefail

DEST="${1:?destination required}"
WAIT="${SIM_WAIT_SECONDS:-300}"
POLL="${SIM_POLL_SECONDS:-10}"

# Only name-based simulator destinations need resolving; id= or device
# destinations are left to xcodebuild.
NAME="$(printf '%s\n' "$DEST" | tr ',' '\n' | sed -n 's/^name=//p')"
OS="$(printf '%s\n' "$DEST" | tr ',' '\n' | sed -n 's/^OS=//p')"
case "$DEST" in *"iOS Simulator"*) ;; *) NAME="" ;; esac
if [ -z "$NAME" ]; then
  echo "No simulator name in destination; skipping wait."
  exit 0
fi

# Prints the UDID of the matching device on the newest iOS runtime (what
# OS=latest selects), or on the runtime whose version starts with $OS.
find_udid() {
  xcrun simctl list devices available -j 2>/dev/null | NAME="$NAME" OS="${OS:-latest}" python3 -c '
import json, os, re, sys
name, want = os.environ["NAME"], os.environ["OS"]
try:
    devices = json.load(sys.stdin)["devices"]
except Exception:
    sys.exit(0)
best = None
for runtime, devs in devices.items():
    m = re.search(r"\.iOS-(\d+(?:-\d+)*)$", runtime)
    if not m:
        continue
    version = tuple(int(p) for p in m.group(1).split("-"))
    if want != "latest" and not ".".join(map(str, version)).startswith(want):
        continue
    for d in devs:
        if d.get("name") == name and d.get("isAvailable", True):
            if best is None or version > best[0]:
                best = (version, d["udid"])
if best:
    print(best[1])
'
}

deadline=$(( $(date +%s) + WAIT ))
udid=""
while :; do
  udid="$(find_udid || true)"
  [ -n "$udid" ] && break
  if [ "$(date +%s)" -ge "$deadline" ]; then
    echo "::error::Simulator '$NAME' (OS=${OS:-latest}) not available after ${WAIT}s."
    echo "--- xcrun simctl list runtimes"
    xcrun simctl list runtimes || true
    echo "--- xcrun simctl list devices available"
    xcrun simctl list devices available || true
    exit 1
  fi
  echo "Waiting for simulator '$NAME' (OS=${OS:-latest})..."
  sleep "$POLL"
done

echo "Found '$NAME': $udid; booting."
xcrun simctl bootstatus "$udid" -b
