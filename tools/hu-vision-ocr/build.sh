#!/usr/bin/env bash
# build.sh <out_dir> — build hu-vision-ocr into <out_dir> (macOS only).
#
# Called by scripts/install-local-vision.sh. Not part of the CMake build, so
# Linux CI never sees Swift: on any other OS, or without swiftc, this prints
# one line and exits 0 (local vision then runs caption-only, and any quoted
# text in the caption is cut because nothing can confirm it).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:?usage: build.sh <out_dir>}"
if [[ "$(uname)" != "Darwin" ]]; then
    echo "hu-vision-ocr: skipped (not macOS)"
    exit 0
fi
if ! command -v swiftc >/dev/null 2>&1; then
    echo "hu-vision-ocr: skipped (no swiftc; install Xcode Command Line Tools)"
    exit 0
fi
mkdir -p "$out"
staged="$out/hu-vision-ocr.staged-$$"
swiftc -O -target "$(uname -m)-apple-macos14" "$here/main.swift" -o "$staged"
chmod 0755 "$staged"
mv -f "$staged" "$out/hu-vision-ocr" # atomic: never cp over a running binary
echo "hu-vision-ocr: built $out/hu-vision-ocr"
