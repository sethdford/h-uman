#!/usr/bin/env bash
# run-smoke-test.sh — Smoke-test for the dev-preset config check in
# scripts/check-dead-strip-ratchet.sh
#
# Each case is a fake build dir holding ONLY a CMakeCache.txt, pointed at via
# HU_BUILD_DIR. The caches are GENERATED from CMakePresets.json's dev preset
# at run time rather than committed, so a preset edit cannot silently turn the
# "matching" case into a mismatch (or vice versa). Everything runs with
# HU_DEAD_STRIP_STRICT=1, because the point is that strict mode does NOT
# override a configuration mismatch.
#
# Cases:
#   match         — cache == dev preset. Must get PAST the config check: it
#                   then skips on the next guard (no link.txt on macOS,
#                   unsupported platform on Linux), never "not the dev preset".
#   bool-spelling — the preset's ON/OFF written as TRUE/0. CMake treats these
#                   as equal, so this is a match too.
#   all-channels  — HU_ENABLE_ALL_CHANNELS=ON, the 2026-09-26 false positive
#                   (1,026 members, A=33/B=96 vs ceilings 31/76). Must skip.
#   unset         — HU_ENABLE_ML missing from the cache (build dir not
#                   configured from the preset). Must skip, naming <unset>.
#   many          — five mismatches. The reason names three, then "+2 more".
#   no-cache      — empty build dir. Must skip.
#
# Every skip must exit 0 and must leave the script's baselines untouched even
# with HU_RATCHET_FROM_HOOK=1 (the only env under which auto-lock may write).
#
# Exit codes:
#   0  — every case behaved as expected
#   1  — one or more cases failed
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/check-dead-strip-ratchet.sh"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/hu-dead-strip-config.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

# make_cache <dir> [VAR=VALUE ...]  — dev-preset cache with overrides;
# VALUE "__unset__" drops the variable.
make_cache() {
    local dir="$TMP/$1"; shift
    mkdir -p "$dir"
    python3 - "$REPO_ROOT/CMakePresets.json" "$dir/CMakeCache.txt" "$@" <<'PYEOF'
import json, sys
presets_path, out_path, *overrides = sys.argv[1:]
by_name = {p["name"]: p for p in json.load(open(presets_path))["configurePresets"]}
def resolved(name):
    p, merged = by_name[name], {}
    parents = p.get("inherits") or []
    for parent in parents if isinstance(parents, list) else [parents]:
        merged.update(resolved(parent))
    merged.update(p.get("cacheVariables", {}))
    return merged
vals = {k: (v["value"] if isinstance(v, dict) else str(v)) for k, v in resolved("dev").items()}
for o in overrides:
    k, v = o.split("=", 1)
    if v == "__unset__":
        vals.pop(k, None)
    else:
        vals[k] = v
with open(out_path, "w") as f:
    f.write("# This is the CMakeCache file.\n")
    for k, v in sorted(vals.items()):
        t = "STRING" if k == "CMAKE_BUILD_TYPE" else "BOOL"
        f.write(f"{k}:{t}={v}\n")
PYEOF
}

make_cache match
make_cache bool-spelling HU_ENABLE_ASAN=TRUE HU_ENABLE_ALL_CHANNELS=0
make_cache all-channels  HU_ENABLE_ALL_CHANNELS=ON
make_cache unset         HU_ENABLE_ML=__unset__
make_cache many          HU_ENABLE_ALL_CHANNELS=ON HU_ENABLE_ASAN=OFF HU_ENABLE_ML=OFF \
                         HU_ENABLE_SQLITE=OFF CMAKE_BUILD_TYPE=Release
mkdir -p "$TMP/no-cache"

before=$(cksum < "$SCRIPT")
FAIL=0

# expect <case> <must-contain> [must-not-contain]
expect() {
    local c="$1" want="$2" not="${3:-}" got=0 out
    out=$(HU_BUILD_DIR="$TMP/$c" HU_DEAD_STRIP_STRICT=1 HU_RATCHET_FROM_HOOK=1 \
          bash "$SCRIPT" 2>/dev/null) || got=$?
    local ok=1
    [ "$got" -eq 0 ] || ok=0
    printf '%s\n' "$out" | grep -qF -- "$want" || ok=0
    if [ -n "$not" ] && printf '%s\n' "$out" | grep -qF -- "$not"; then ok=0; fi
    if [ "$ok" = 1 ]; then
        echo "PASS  $c → exit $got, '$want'"
    else
        local also=""
        [ -n "$not" ] && also=", without '$not'"
        echo "FAIL  $c → exit $got (expected 0 with '$want'$also)" >&2
        printf '%s\n' "$out" | sed 's/^/      /' >&2
        FAIL=1
    fi
}

# The guard right after the config check: platform on Linux, link.txt on macOS.
# Reaching it is what "proceeds" means for a build dir with no build in it.
if [ "$(uname -s)" = "Darwin" ]; then
    next_guard="CMakeFiles/human.dir/link.txt"
else
    next_guard="unsupported platform"
fi
expect match         "$next_guard" "not the dev preset"
expect bool-spelling "$next_guard" "not the dev preset"
expect all-channels  "not the dev preset (HU_ENABLE_ALL_CHANNELS=ON vs preset OFF); reconfigure with: cmake --preset dev"
expect unset         "HU_ENABLE_ML=<unset> vs preset ON"
expect many          "; +2 more); reconfigure with: cmake --preset dev"
expect no-cache      "RATCHET_SKIP: no $TMP/no-cache/CMakeCache.txt"

if [ "$(cksum < "$SCRIPT")" != "$before" ]; then
    echo "FAIL  a skipped run rewrote $SCRIPT (baseline locked from an unmeasured build)" >&2
    FAIL=1
fi

if [ $FAIL -gt 0 ]; then
    exit 1
fi

echo "OK  check-dead-strip-ratchet config-check smoke test passed"
exit 0
