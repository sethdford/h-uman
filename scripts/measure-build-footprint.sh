#!/usr/bin/env bash
# Measure one build configuration's footprint and print it, with provenance, as
# JSON on stdout. Every footprint number in the docs must come from a run of
# this script (see .claude/rules/no-number-without-a-measurement.md) and name
# the configuration it describes — the release preset and the CI release-size
# build differ by several hundred KB, so "the release binary" is ambiguous.
#
# Usage:
#   scripts/measure-build-footprint.sh <name> <build-dir> --preset <preset>
#   scripts/measure-build-footprint.sh <name> <build-dir> -- <cmake -D args...>
#
# Measured:
#   binary_bytes            size of <build-dir>/human
#   startup_ms_median       median wall time of `human --version`, 10 warm runs
#   version_peak_rss_bytes  peak RSS of `human --version` (/usr/bin/time -l) —
#                           the metric behind the historical "<6 MB RAM" claim
#   idle_rss_bytes          steady-state RSS of `human mcp` blocked on stdin with
#                           an isolated HOME, HU_STATE_DIR and cwd. `--version`
#                           exits in milliseconds and never idles, so its peak
#                           understates a running process. Method taken from
#                           scripts/measure-footprint.sh on website-2a-shell-homepage
#                           (8cb3df597), including its cwd isolation: config_merge.c
#                           merges <cwd>/.human/config.json regardless of HOME.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

[ $# -ge 3 ] || { sed -n '8,10p' "$0" >&2; exit 2; }
NAME="$1"
BUILD="$2"
shift 2
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

if [ "$1" = "--preset" ]; then
    PRESET="$2"
    CONFIGURE="cmake --preset $PRESET"
    cmake --preset "$PRESET" -S "$ROOT" >/dev/null
    cmake --build --preset "$PRESET" --target human -j8 >/dev/null
elif [ "$1" = "--" ]; then
    shift
    CONFIGURE="cmake -B <build> $*"
    cmake -S "$ROOT" -B "$BUILD" "$@" >/dev/null
    cmake --build "$BUILD" --target human -j8 >/dev/null
else
    echo "measure-build-footprint: expected --preset <p> or -- <cmake args>" >&2
    exit 2
fi
BIN="$BUILD/human"
[ -x "$BIN" ] || { echo "measure-build-footprint: $BIN missing after build" >&2; exit 2; }
BUILD_TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$BUILD/CMakeCache.txt")"

# ── Idle RSS ────────────────────────────────────────────────────────────────
TMP_HOME="$(mktemp -d)"
FIFO="$TMP_HOME/stdin.fifo"
MCP_PID=""
cleanup() {
    local status=$?
    if [ -n "$MCP_PID" ] && kill -0 "$MCP_PID" 2>/dev/null; then
        kill "$MCP_PID" 2>/dev/null || true
        wait "$MCP_PID" 2>/dev/null || true
    fi
    exec 3<&- 2>/dev/null || true
    rm -rf "$TMP_HOME"
    exit "$status"
}
trap cleanup EXIT

mkfifo "$FIFO"
exec 3<>"$FIFO"
(
    cd "$TMP_HOME" &&
        exec env -i HOME="$TMP_HOME" HU_STATE_DIR="$TMP_HOME/.human" PATH="$PATH" \
            "$BIN" mcp <&3 >/dev/null 2>/dev/null
) &
MCP_PID=$!

# Stable once 3 consecutive samples are within 2% of each other.
is_stable() {
    awk -v a="$1" -v b="$2" -v c="$3" 'BEGIN {
        mx = a; if (b > mx) mx = b; if (c > mx) mx = c;
        mn = a; if (b < mn) mn = b; if (c < mn) mn = c;
        if (mn <= 0) exit 1;
        exit (((mx - mn) / mn) <= 0.02) ? 0 : 1;
    }'
}
idle_rss_kib=""
prev1=""
prev2=""
elapsed_ms=0
while [ "$elapsed_ms" -lt 20000 ]; do
    sleep 0.25
    elapsed_ms=$((elapsed_ms + 250))
    kill -0 "$MCP_PID" 2>/dev/null || { echo "measure-build-footprint: human mcp exited early" >&2; exit 2; }
    cur="$(ps -o rss= -p "$MCP_PID" 2>/dev/null | tr -d ' ')"
    [ -n "$cur" ] || continue
    if [ -n "$prev1" ] && [ -n "$prev2" ] && is_stable "$prev2" "$prev1" "$cur"; then
        idle_rss_kib="$cur"
        break
    fi
    prev2="$prev1"
    prev1="$cur"
done
[ -n "$idle_rss_kib" ] || { echo "measure-build-footprint: idle RSS did not stabilize within 20s" >&2; exit 2; }
kill "$MCP_PID" 2>/dev/null || true
wait "$MCP_PID" 2>/dev/null || true
MCP_PID=""

# ── Peak RSS of --version (macOS /usr/bin/time -l reports bytes) ────────────
version_peak=""
if [ "$(uname -s)" = "Darwin" ]; then
    version_peak="$( (cd "$TMP_HOME" && env -i HOME="$TMP_HOME" HU_STATE_DIR="$TMP_HOME/.human" PATH="$PATH" \
        /usr/bin/time -l "$BIN" --version) 2>&1 >/dev/null | awk '/maximum resident set size/ {print $1}')"
fi

python3 - "$BIN" "$NAME" "$CONFIGURE" "$BUILD_TYPE" "$(git -C "$ROOT" rev-parse --short HEAD)" \
    "$(git -C "$ROOT" status --porcelain -- src include CMakeLists.txt CMakePresets.json | wc -l | tr -d ' ')" \
    "$idle_rss_kib" "${version_peak:-}" <<'PY'
import json, os, platform, statistics, subprocess, sys, time, datetime
binp, name, configure, build_type, rev, dirty, idle_kib, vpeak = sys.argv[1:9]
times = []
for _ in range(11):
    t = time.perf_counter()
    subprocess.run([binp, "--version"], check=True, capture_output=True)
    times.append((time.perf_counter() - t) * 1000)
times = times[1:]  # drop the cold first run
print(json.dumps({
    "name": name,
    "configure": configure,
    "cmake_build_type": build_type,
    "binary_bytes": os.path.getsize(binp),
    "startup_ms_median": round(statistics.median(times), 1),
    "startup_ms_max": round(max(times), 1),
    "version_peak_rss_bytes": int(vpeak) if vpeak else None,
    "idle_rss_bytes": int(round(float(idle_kib) * 1024)),
    "git_rev": rev,
    "source_tree_dirty": dirty != "0",
    "measured_at": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
    "platform": f"{platform.system()} {platform.machine()} {platform.mac_ver()[0] or platform.release()}",
    "startup_command": "human --version (10 warm runs, median)",
    "idle_command": "human mcp (isolated HOME + HU_STATE_DIR + cwd, no config, stdin held open)",
}, indent=2))
PY
