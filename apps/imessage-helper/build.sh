#!/usr/bin/env bash
# Build the iMessage IMCore helper dylib.
#
#   bash apps/imessage-helper/build.sh                 # dev/compile-check (host arch)
#   bash apps/imessage-helper/build.sh --arm64e        # production arch (matches Messages.app)
#   bash apps/imessage-helper/build.sh --arm64e --sign "Developer ID Application: …"
#
# Emits build/imessage-helper/libIMHelper.dylib with an `-init __dylib_init`
# constructor that fires when DYLD_INSERT_LIBRARIES loads it into a process.
#
# All IMCore access is via dlsym/NSClassFromString at runtime, so there are NO
# link-time private-framework dependencies — this compiles and links anywhere a
# Swift toolchain exists. Runtime behavior requires SIP disabled (see README).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$HERE/Sources/IMHelper"
OUT_DIR="${OUT_DIR:-$HERE/../../build/imessage-helper}"
OUT="$OUT_DIR/libIMHelper.dylib"

TARGET_FLAGS=()
SIGN_IDENTITY=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --arm64e) TARGET_FLAGS=(-target "arm64e-apple-macos14.0"); shift ;;
    --sign)   SIGN_IDENTITY="${2:-}"; shift 2 ;;
    *) echo "unknown flag: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "$OUT_DIR"

# -swift-version 5 avoids Swift 6 strict-concurrency errors on the global
# mutable state (tcp/resolved_* singletons) this runtime-injection design uses.
swiftc \
  -emit-library \
  -module-name IMHelper \
  -swift-version 5 \
  "${TARGET_FLAGS[@]}" \
  -framework Foundation \
  -framework Network \
  -Xlinker -init -Xlinker __dylib_init \
  -o "$OUT" \
  "$SRC_DIR"/*.swift

echo "built: $OUT"

if [[ -n "$SIGN_IDENTITY" ]]; then
  codesign --force --sign "$SIGN_IDENTITY" --timestamp=none "$OUT"
  echo "signed with: $SIGN_IDENTITY"
fi
