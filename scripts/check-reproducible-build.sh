#!/usr/bin/env bash
# check-reproducible-build.sh
#
# US-8.3 — Reproducible Binary Builds. Builds the `human` binary twice
# from a clean checkout into two distinct build directories with
# SOURCE_DATE_EPOCH pinned, then verifies the two binaries are
# bytewise-identical modulo a documented set of carve-outs.
#
# AC-8.3.1: two builds with same SOURCE_DATE_EPOCH from same source → same SHA-256.
# AC-8.3.2: CI runs this script on linux x86_64 → exits 0.
# AC-8.3.4: same source, different absolute build paths → same SHA-256.
#
# Exit codes:
#   0  binaries match after carve-outs (or are identical raw)
#   1  binaries differ on a non-carved-out section (CONTRACT VIOLATED)
#   2  invocation error or precondition not met (e.g. missing tool)
#
# Anti-pattern AP-1 from sprints/sprint-8/designs/US-8.3.md: a script
# that prints "diff found" and exits 0 is the failure mode this
# story is supposed to prevent. This script MUST fail loudly.
#
# Anti-pattern AP-2: do NOT skip comparison when SOURCE_DATE_EPOCH is
# unset. This script sets it explicitly so the gate fires regardless
# of the caller's environment.
#
# Carve-outs (documented, narrow):
#   - Linux ELF: .note.gnu.build-id section. We pass -Wl,--build-id=none
#     in CMakeLists.txt so this section should be absent, but if a
#     linker version still emits it we strip via objcopy before cmp.
#   - macOS Mach-O: LC_UUID load command. We pass -Wl,-no_uuid so this
#     should be zero, but a hex-stripping pass is the safety net.
#   - Code-signature blobs (Darwin only): ad-hoc codesign adds a CMS
#     signature whose timestamp varies. CMakeLists.txt for the `human`
#     target invokes codesign on Darwin dev builds; we strip the
#     signature segment before comparison.
# All carve-outs are NARROWLY scoped to the specific section/command;
# we do NOT compare just the .text section or "the file minus those".
# We compare the full binary after the carve-out scrub. If the scrub
# leaves any byte differing, the script fails.

set -euo pipefail

# Fixed epoch (2023-11-14T22:13:20Z). Arbitrary but stable. Matches the
# value in sprints/sprint-8/stories.md AC-8.3.1.
EPOCH="${SOURCE_DATE_EPOCH:-1700000000}"
export SOURCE_DATE_EPOCH="$EPOCH"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

VERBOSE=0
PRESET="release"
KEEP_DIRS=0

usage() {
    cat <<'EOF'
Usage: check-reproducible-build.sh [--verbose] [--keep-dirs] [--preset NAME]

Builds `human` twice from a clean checkout into two distinct directories
under different parent paths, with SOURCE_DATE_EPOCH=1700000000, then
diffs the resulting binaries. Exits 0 only if the binaries match
(after documented carve-outs). Exits 1 on any mismatch.

Options:
  --verbose      Print SHA-256, objdump -h, and first-diff hexdump on
                 failure (and on success when given).
  --keep-dirs    Do not remove build-rep-a/ and build-rep-b/ on success.
                 Useful for diagnosing flake.
  --preset NAME  CMake preset to build (default: release).
  --help         Show this help.

This is the gate for US-8.3 AC-8.3.1, AC-8.3.2, AC-8.3.4.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --verbose) VERBOSE=1; shift ;;
        --keep-dirs) KEEP_DIRS=1; shift ;;
        --preset) PRESET="${2:?--preset requires an argument}"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

cd "$REPO_ROOT"

# --- Pick the build dirs under different parent paths --------------------
# AC-8.3.4 wants different ABSOLUTE paths between the two builds, so we
# use mktemp under TMPDIR/HOME to ensure the prefixes differ. CMake's
# build dirs cannot share a parent or we'd defeat the test.
TMP_PARENT_A="$(mktemp -d -t hu-rep-a.XXXXXX)"
TMP_PARENT_B="$(mktemp -d -t hu-rep-b.XXXXXX)"
BUILD_A="$TMP_PARENT_A/build"
BUILD_B="$TMP_PARENT_B/build"

cleanup() {
    if [[ $KEEP_DIRS -eq 0 ]]; then
        rm -rf "$TMP_PARENT_A" "$TMP_PARENT_B" || true
    else
        echo "kept: $BUILD_A and $BUILD_B" >&2
    fi
}
trap cleanup EXIT

build_once() {
    local build_dir="$1"
    local label="$2"
    echo "==> [$label] cmake configure → $build_dir" >&2
    # Run cmake from the repo root but with -B pointing at the off-tree
    # build dir. Use the same preset both times; that way every flag in
    # CMakePresets.json (build type, channel list, etc.) is identical.
    SOURCE_DATE_EPOCH="$EPOCH" cmake \
        -S "$REPO_ROOT" -B "$build_dir" \
        --preset "$PRESET" \
        -DCMAKE_BINARY_DIR_OVERRIDE_UNUSED=1 \
        >"$build_dir.cfg.log" 2>&1 || {
        # The preset already pins binaryDir, so passing -B may conflict.
        # Retry without the preset binaryDir by using a fresh non-preset
        # invocation that mirrors the preset's flags. For now we just
        # surface the configure log.
        echo "cmake configure failed; log follows:" >&2
        cat "$build_dir.cfg.log" >&2
        return 1
    }
    echo "==> [$label] cmake build → human" >&2
    SOURCE_DATE_EPOCH="$EPOCH" cmake --build "$build_dir" --target human -j \
        >"$build_dir.build.log" 2>&1 || {
        echo "cmake build failed; tail of log follows:" >&2
        tail -100 "$build_dir.build.log" >&2
        return 1
    }
}

# CMake presets pin binaryDir, so -B is ignored when --preset is passed.
# We instead invoke cmake without --preset and replicate the release flags
# directly so the build dir is controllable.
build_once_no_preset() {
    local build_dir="$1"
    local label="$2"
    mkdir -p "$build_dir"
    echo "==> [$label] cmake configure → $build_dir (SOURCE_DATE_EPOCH=$EPOCH)" >&2
    SOURCE_DATE_EPOCH="$EPOCH" cmake \
        -S "$REPO_ROOT" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DHU_ENABLE_LTO=ON \
        -DHU_ENABLE_ALL_CHANNELS=ON \
        -DHU_ENABLE_SQLITE=ON \
        -DHU_ENABLE_PERSONA=ON \
        -DHU_ENABLE_SKILLS=ON \
        -DHU_ENABLE_FEEDS=ON \
        -DHU_ENABLE_UPDATE=ON \
        -DHU_ENABLE_ML=ON \
        -DHU_ENABLE_EMBEDDED_MODEL=ON \
        -DHU_ENABLE_CARTESIA=ON \
        -DHU_ENABLE_CRON=ON \
        -DHU_ENABLE_TOOLS_ADVANCED=ON \
        >"$build_dir/cfg.log" 2>&1 || {
        echo "cmake configure failed; log follows:" >&2
        cat "$build_dir/cfg.log" >&2
        return 1
    }
    echo "==> [$label] cmake build → human (SOURCE_DATE_EPOCH=$EPOCH)" >&2
    SOURCE_DATE_EPOCH="$EPOCH" cmake --build "$build_dir" --target human -j \
        >"$build_dir/build.log" 2>&1 || {
        echo "cmake build failed; tail of log follows:" >&2
        tail -100 "$build_dir/build.log" >&2
        return 1
    }
}

build_once_no_preset "$BUILD_A" "A"
build_once_no_preset "$BUILD_B" "B"

BIN_A="$BUILD_A/human"
BIN_B="$BUILD_B/human"

if [[ ! -x "$BIN_A" ]]; then echo "error: $BIN_A not built" >&2; exit 2; fi
if [[ ! -x "$BIN_B" ]]; then echo "error: $BIN_B not built" >&2; exit 2; fi

# --- Apply carve-outs ----------------------------------------------------
# We modify COPIES of the binaries; the originals stay for diagnostics.
SCRUB_A="$TMP_PARENT_A/human.scrub"
SCRUB_B="$TMP_PARENT_B/human.scrub"
cp "$BIN_A" "$SCRUB_A"
cp "$BIN_B" "$SCRUB_B"

UNAME="$(uname -s)"
if [[ "$UNAME" == "Linux" ]]; then
    # Strip .note.gnu.build-id if present. -Wl,--build-id=none should
    # have prevented it, but if a packaged toolchain re-adds it, scrub.
    if command -v objcopy >/dev/null 2>&1; then
        objcopy --remove-section=.note.gnu.build-id "$SCRUB_A" 2>/dev/null || true
        objcopy --remove-section=.note.gnu.build-id "$SCRUB_B" 2>/dev/null || true
    fi
elif [[ "$UNAME" == "Darwin" ]]; then
    # Mach-O: -Wl,-no_uuid in CMakeLists.txt zeroes LC_UUID. The ad-hoc
    # codesign performed by the human target's POST_BUILD step adds a
    # CMS signature with a hash that varies because the binary content
    # downstream of the signature embeds the signature itself.
    # We strip both load commands via codesign --remove-signature.
    # That makes LC_CODE_SIGNATURE absent in both copies. LC_UUID is
    # already zero from -no_uuid.
    if command -v codesign >/dev/null 2>&1; then
        codesign --remove-signature "$SCRUB_A" 2>/dev/null || true
        codesign --remove-signature "$SCRUB_B" 2>/dev/null || true
    fi
fi

# --- Compare -------------------------------------------------------------
SHA_A=$(shasum -a 256 "$SCRUB_A" | awk '{print $1}')
SHA_B=$(shasum -a 256 "$SCRUB_B" | awk '{print $1}')

echo "build A: $BIN_A  sha256(scrubbed)=$SHA_A"
echo "build B: $BIN_B  sha256(scrubbed)=$SHA_B"

if [[ "$SHA_A" == "$SHA_B" ]]; then
    echo "OK: reproducible build verified (SHA-256 matches after carve-outs)"
    if [[ $VERBOSE -eq 1 ]]; then
        echo "raw size A: $(stat -f%z "$BIN_A" 2>/dev/null || stat -c%s "$BIN_A")"
        echo "raw size B: $(stat -f%z "$BIN_B" 2>/dev/null || stat -c%s "$BIN_B")"
    fi
    exit 0
fi

# Mismatch — diagnostic dump, then FAIL LOUDLY.
echo "FAIL: binaries differ after carve-outs" >&2
echo "  A: $SCRUB_A" >&2
echo "  B: $SCRUB_B" >&2

if command -v cmp >/dev/null 2>&1; then
    echo "first 32 differing bytes:" >&2
    cmp -l "$SCRUB_A" "$SCRUB_B" 2>&1 | head -32 >&2 || true
fi

if [[ $VERBOSE -eq 1 ]]; then
    if [[ "$UNAME" == "Linux" ]] && command -v objdump >/dev/null 2>&1; then
        echo "--- objdump -h A ---" >&2
        objdump -h "$BIN_A" >&2 || true
        echo "--- objdump -h B ---" >&2
        objdump -h "$BIN_B" >&2 || true
    elif [[ "$UNAME" == "Darwin" ]] && command -v otool >/dev/null 2>&1; then
        echo "--- otool -l A (first 80 lines) ---" >&2
        otool -l "$BIN_A" | head -80 >&2 || true
        echo "--- otool -l B (first 80 lines) ---" >&2
        otool -l "$BIN_B" | head -80 >&2 || true
    fi
fi

cat >&2 <<'EOF'

US-8.3 reproducibility contract VIOLATED.

The two binaries differ on at least one byte after the documented
carve-outs (ELF .note.gnu.build-id on Linux, Mach-O LC_UUID +
ad-hoc code signature on Darwin) were applied.

Diagnostic next steps (from sprints/sprint-8/designs/US-8.3.md
"Failure recovery"):
  1. cmp -l <A> <B> | head -20   # byte offsets
  2. objdump -h <A> vs objdump -h <B>   # which section
  3. Common culprits, in order:
     (a) a new generated file embeds a timestamp,
     (b) a new use of __DATE__/__TIME__ slipped past the gate
         (shouldn't, given -Werror=date-time),
     (c) toolchain on CI updated (check `apt list --installed gcc`),
     (d) a new dependency embeds its own build ID.

DO NOT silence this script. Fix the root cause.
EOF
exit 1
