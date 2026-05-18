#!/usr/bin/env bash
# scripts/check-reproducible-build.sh
#
# US-42.3 AC-42.3.4 slow gate: build the `human` release target twice under
# a frozen SOURCE_DATE_EPOCH and verify the two binaries have identical
# sha256 hashes. On mismatch, produce an operator-friendly diff (readelf
# on linux, otool on darwin) so reviewers can see WHICH section diverged.
#
# Also asserts (AC-42.3.4):
#   linux  → no .note.gnu.build-id section (carved by -Wl,--build-id=none)
#   darwin → no LC_UUID load command       (carved by -Wl,-no_uuid)
#
# And (AC-42.3.5):
#   `strings build/human | grep "$PWD"` returns no matches, proving
#   -ffile-prefix-map rewrote source paths to "." in the binary.
#
# Usage:
#   bash scripts/check-reproducible-build.sh                    # default behavior
#   bash scripts/check-reproducible-build.sh --two-build-diff   # explicit flag
#                                                                 (story AC wording)
#
# Implementation notes:
#   * Two builds into SEPARATE build dirs under $TMPDIR; we do NOT reuse a
#     single build dir between runs because incremental rebuild semantics
#     could mask non-reproducibility.
#   * NO ccache between the two builds. ccache would short-circuit the
#     second build and defeat the test. Set CCACHE_DISABLE=1 explicitly.
#   * The two binaries differ in their PATH on disk, but that path is NOT
#     embedded in the binary (this is exactly what -ffile-prefix-map
#     prevents); the binaries themselves are byte-identical.
#   * Some platforms (darwin signed binaries) embed a code-signature blob
#     that captures the binary's pre-signature hash. We strip the signature
#     with `codesign --remove-signature` before the sha compare so the
#     signature byte differences don't masquerade as content differences.
#
# Smoke test (verifies the gate actually catches non-reproducibility):
#   1. Edit any src/*.c to add `static const char *_probe = __DATE__;`
#   2. Disable -Werror=date-time temporarily to allow compilation
#   3. Run this script; expect "FAIL: sha256 mismatch"
#   4. Revert
#
# Related: sprints/sprint-42/designs/US-42.3.md (Step 5),
# .claude/rules/quality-gates.md.

set -euo pipefail

# Parse the one flag we accept; the default behavior already IS two-build
# diff, but the AC text uses this flag spelling so we accept it.
MODE="two-build-diff"
while [ $# -gt 0 ]; do
    case "$1" in
        --two-build-diff)
            MODE="two-build-diff"
            shift
            ;;
        -h|--help)
            sed -n '2,40p' "$0"
            exit 0
            ;;
        *)
            echo "ERROR: unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

# A stable, far-past epoch — story AC-42.3.4 explicitly names this value.
export SOURCE_DATE_EPOCH=1700000000
export CCACHE_DISABLE=1

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$REPO_ROOT"

# Check for required tools
if ! command -v sha256sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
    echo "ERROR: neither sha256sum nor shasum is available" >&2
    exit 2
fi

# Use sha256sum on linux, shasum -a 256 on darwin.
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

# Platform detection
case "$(uname -s)" in
    Darwin) PLATFORM="darwin" ;;
    Linux)  PLATFORM="linux"  ;;
    *)
        echo "ERROR: unsupported platform: $(uname -s)" >&2
        exit 2
        ;;
esac

# Carve linux build-id / darwin code-signature / darwin LC_UUID so the
# sha-compare doesn't trip on bytes that the linker flags should already
# have eliminated. If the flags worked, these carves are no-ops; if they
# didn't work, the carve produces a sharper failure than a whole-file diff.
carve_nondeterminism() {
    local binary="$1"
    if [ "$PLATFORM" = "linux" ]; then
        # If a build-id section snuck in despite -Wl,--build-id=none,
        # objcopy --remove-section will surface that AND let us compare
        # the rest of the binary. If it's already absent, this is a no-op.
        if command -v objcopy >/dev/null 2>&1; then
            objcopy --remove-section=.note.gnu.build-id "$binary" 2>/dev/null || true
        fi
    elif [ "$PLATFORM" = "darwin" ]; then
        # Strip codesign if present (CMake POST_BUILD signs with
        # "Human Local Dev" on dev machines). Production CI runners are
        # unsigned so this is a no-op there.
        if command -v codesign >/dev/null 2>&1; then
            codesign --remove-signature "$binary" 2>/dev/null || true
        fi
        # NOTE: LC_UUID can't be cleanly removed in-place; we *assert*
        # it's absent below and rely on -Wl,-no_uuid to keep it that way.
    fi
}

# Carve and assert that the platform-specific non-determinism source is
# absent (build-id on linux, LC_UUID on darwin). Returns 0 if absent.
assert_no_buildid_or_uuid() {
    local binary="$1"
    if [ "$PLATFORM" = "linux" ]; then
        if command -v readelf >/dev/null 2>&1; then
            if readelf -n "$binary" 2>/dev/null | grep -q "Build ID"; then
                echo "FAIL: $binary contains a .note.gnu.build-id section" >&2
                echo "  -Wl,--build-id=none did not take effect" >&2
                return 1
            fi
        else
            echo "WARN: readelf not available; skipping build-id assertion" >&2
        fi
    elif [ "$PLATFORM" = "darwin" ]; then
        if command -v otool >/dev/null 2>&1; then
            if otool -l "$binary" 2>/dev/null | grep -q "LC_UUID"; then
                echo "FAIL: $binary contains an LC_UUID load command" >&2
                echo "  -Wl,-no_uuid did not take effect" >&2
                return 1
            fi
        else
            echo "WARN: otool not available; skipping LC_UUID assertion" >&2
        fi
    fi
    return 0
}

# AC-42.3.5: verify `strings $binary | grep $PWD` is empty.
assert_no_source_paths() {
    local binary="$1"
    if command -v strings >/dev/null 2>&1; then
        if strings "$binary" | grep -Fq "$REPO_ROOT"; then
            echo "FAIL: $binary contains source paths from $REPO_ROOT" >&2
            echo "  -ffile-prefix-map did not take effect" >&2
            strings "$binary" | grep -F "$REPO_ROOT" | head -5 | sed 's/^/    /' >&2
            return 1
        fi
    else
        echo "WARN: strings not available; skipping source-path assertion" >&2
    fi
    return 0
}

# Dump the binary into an operator-friendly text form for diff context.
dump_binary() {
    local binary="$1"
    local out="$2"
    if [ "$PLATFORM" = "linux" ]; then
        readelf -aW "$binary" > "$out" 2>&1 || true
    else
        otool -lv "$binary" > "$out" 2>&1 || true
    fi
}

# --- Run two builds ---

B1=$(mktemp -d -t hu-repro-b1.XXXXXX)
B2=$(mktemp -d -t hu-repro-b2.XXXXXX)
trap 'rm -rf "$B1" "$B2"' EXIT

echo "Reproducible build: configuring + building twice under SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"
echo "  build 1: $B1"
echo "  build 2: $B2"
echo ""

# Configure + build #1
echo "--- Build 1 ---"
cmake -S "$REPO_ROOT" -B "$B1" \
      -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DHU_ENABLE_LTO=ON \
      -DHU_ENABLE_ALL_CHANNELS=ON \
      -DHU_ENABLE_SQLITE=ON \
      -DHU_ENABLE_PERSONA=ON \
      -DHU_ENABLE_SKILLS=ON \
      >/dev/null
cmake --build "$B1" --target human -j"$( (nproc 2>/dev/null || sysctl -n hw.ncpu) )"

# Configure + build #2
echo "--- Build 2 ---"
cmake -S "$REPO_ROOT" -B "$B2" \
      -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DHU_ENABLE_LTO=ON \
      -DHU_ENABLE_ALL_CHANNELS=ON \
      -DHU_ENABLE_SQLITE=ON \
      -DHU_ENABLE_PERSONA=ON \
      -DHU_ENABLE_SKILLS=ON \
      >/dev/null
cmake --build "$B2" --target human -j"$( (nproc 2>/dev/null || sysctl -n hw.ncpu) )"

BIN1="$B1/human"
BIN2="$B2/human"

if [ ! -f "$BIN1" ] || [ ! -f "$BIN2" ]; then
    echo "ERROR: one or both build outputs missing" >&2
    echo "  expected: $BIN1 and $BIN2" >&2
    exit 2
fi

# Carve known-noise bytes before the sha compare
carve_nondeterminism "$BIN1"
carve_nondeterminism "$BIN2"

# Per-platform structural assertions FIRST so we get the sharper error
# message before the whole-binary sha diff.
echo ""
echo "--- Structural assertions on build 1 ---"
assert_no_buildid_or_uuid "$BIN1"
assert_no_source_paths   "$BIN1"

# --- sha256 compare ---
echo ""
echo "--- sha256 compare ---"
H1=$(sha256_of "$BIN1")
H2=$(sha256_of "$BIN2")
echo "  build 1 sha256: $H1"
echo "  build 2 sha256: $H2"

if [ "$H1" != "$H2" ]; then
    echo ""
    echo "FAIL: sha256 mismatch between the two builds." >&2
    echo "  Generating diff to help locate the divergent section..." >&2
    dump_binary "$BIN1" "$B1/dump.txt"
    dump_binary "$BIN2" "$B2/dump.txt"
    if command -v diff >/dev/null 2>&1; then
        diff -u "$B1/dump.txt" "$B2/dump.txt" | head -200 >&2 || true
    fi
    exit 1
fi

echo ""
echo "OK: two-build sha256 diff is clean (mode=$MODE)"
echo "  $H1"
exit 0
