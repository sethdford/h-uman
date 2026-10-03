#!/usr/bin/env bash
# build.sh <out_dir> — build hu-calendar-free-busy into <out_dir> (macOS only).
#
# Called by scripts/install-human-daemon.sh. Not part of the CMake build, so
# Linux CI never sees Swift: on any other OS, or without swiftc, this prints
# one line and exits 0 (the commitment guard then reads calendar=unknown).
# The Info.plist is embedded so macOS can show the Calendar usage string.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:?usage: build.sh <out_dir>}"
if [[ "$(uname)" != "Darwin" ]]; then
    echo "calendar-free-busy: skipped (not macOS)"
    exit 0
fi
if ! command -v swiftc >/dev/null 2>&1; then
    echo "calendar-free-busy: skipped (no swiftc; install Xcode Command Line Tools)"
    exit 0
fi
mkdir -p "$out"
staged="$out/hu-calendar-free-busy.staged-$$"
swiftc -O -target "$(uname -m)-apple-macos14" "$here/main.swift" -o "$staged" \
    -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker "$here/Info.plist"
chmod 0755 "$staged"
mv -f "$staged" "$out/hu-calendar-free-busy" # atomic: never cp over a running binary
echo "calendar-free-busy: built $out/hu-calendar-free-busy"
