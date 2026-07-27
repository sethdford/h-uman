#!/usr/bin/env bash
# scripts/test-ratchet-lib.sh — smoke test for scripts/lib/ratchet.sh.
#
# A guard that has never fired is not known to guard
# (.claude/rules/no-number-without-a-measurement.md, "How to verify a guard
# actually guards"). Auto-lock rewrites committed baseline constants and stages
# them, so it needs the opposite proof too: that it stays silent on every path
# where firing would be wrong.
#
# Hermetic: every case runs in a throwaway git repo under $TMPDIR. The real
# scripts/check-*.sh baselines are never touched.
set -uo pipefail

LIB="$(cd "$(dirname "$0")" && pwd)/lib/ratchet.sh"
[ -f "$LIB" ] || { echo "missing $LIB" >&2; exit 1; }

pass=0
fail=0
check() { # check DESC EXPECTED ACTUAL
    if [ "$2" = "$3" ]; then pass=$((pass+1)); printf '  PASS  %s\n' "$1"
    else fail=$((fail+1)); printf '  FAIL  %s (expected %q, got %q)\n' "$1" "$2" "$3"; fi
}

# new_repo -> prints path to a fresh repo containing gate.sh with FOO_BASELINE=100
new_repo() {
    local d; d=$(mktemp -d)
    git -C "$d" init -q
    git -C "$d" config user.email t@t; git -C "$d" config user.name t
    printf 'FOO_BASELINE=100   # seeded\nOTHER=1\n' > "$d/gate.sh"
    git -C "$d" add gate.sh; git -C "$d" commit -qm seed
    echo "$d"
}
baseline_of() { grep -oE '^FOO_BASELINE=[0-9]+' "$1/gate.sh" | cut -d= -f2; }
staged_p()   { git -C "$1" diff --cached --name-only | grep -qx gate.sh && echo yes || echo no; }

echo "ratchet_autolock:"

# 1. Measured BELOW baseline -> tighten and stage. The whole point.
d=$(new_repo); ( cd "$d" && . "$LIB" && ratchet_autolock FOO_BASELINE 90 gate.sh >/dev/null )
check "tightens baseline when the counter drops" "90" "$(baseline_of "$d")"
check "stages the rewritten gate"                "yes" "$(staged_p "$d")"
rm -rf "$d"

# 2. Measured ABOVE baseline -> untouched. Auto-lock must never LOOSEN a gate;
#    that would silently grant the growth the ratchet exists to refuse.
d=$(new_repo); ( cd "$d" && . "$LIB" && ratchet_autolock FOO_BASELINE 150 gate.sh >/dev/null )
check "never loosens on a counter that grew"     "100" "$(baseline_of "$d")"
check "does not stage when nothing changed"      "no"  "$(staged_p "$d")"
rm -rf "$d"

# 3. Equal -> untouched, unstaged. Equilibrium must be a no-op, or every commit
#    would carry a spurious one-line diff.
d=$(new_repo); ( cd "$d" && . "$LIB" && ratchet_autolock FOO_BASELINE 100 gate.sh >/dev/null )
check "no-op at equilibrium"                     "100" "$(baseline_of "$d")"
check "no spurious staging at equilibrium"       "no"  "$(staged_p "$d")"
rm -rf "$d"

# 4. Gate file carries unstaged edits -> refuse. Staging it would sweep an
#    unrelated in-flight change into someone else's commit.
d=$(new_repo); printf 'FOO_BASELINE=100   # seeded\nOTHER=2\n' > "$d/gate.sh"
( cd "$d" && . "$LIB" && ratchet_autolock FOO_BASELINE 90 gate.sh >/dev/null 2>&1 )
check "refuses when the gate has unstaged edits" "100" "$(baseline_of "$d")"
rm -rf "$d"

# 5. Non-numeric measurement -> refuse. A failed measurement must never be
#    persisted as a baseline (no-number-without-a-measurement.md).
for bad in "" "n/a" "12abc"; do
    d=$(new_repo); ( cd "$d" && . "$LIB" && ratchet_autolock FOO_BASELINE "$bad" gate.sh >/dev/null 2>&1 )
    check "refuses non-numeric measurement '${bad}'" "100" "$(baseline_of "$d")"
    rm -rf "$d"
done

# 6. Opt-out honored.
d=$(new_repo)
( cd "$d" && . "$LIB" && HU_RATCHET_NO_AUTOLOCK=1 ratchet_autolock FOO_BASELINE 90 gate.sh >/dev/null )
check "HU_RATCHET_NO_AUTOLOCK=1 disables it"     "100" "$(baseline_of "$d")"
rm -rf "$d"

# 7. Unknown variable name -> no-op, no crash.
d=$(new_repo); ( cd "$d" && . "$LIB" && ratchet_autolock NOPE_BASELINE 90 gate.sh >/dev/null 2>&1 )
check "unknown var is a clean no-op"             "100" "$(baseline_of "$d")"
rm -rf "$d"

echo
echo "ratchet_config_field:"
cfgdir="$(cd "$(dirname "$0")/.." && pwd)"
( cd "$cfgdir" && . "$LIB"
  v=$(ratchet_config_field clone var)
  f=$(ratchet_config_field file-size floor)
  r=$(ratchet_config_field edge-cross-channel rate)
  u=$(ratchet_config_field no-such-ratchet var)
  [ "$v" = "CLONE_BASELINE" ] && echo "  PASS  reads var column"   || { echo "  FAIL  var column: $v"; exit 1; }
  [ "$f" = "800" ]            && echo "  PASS  reads floor column" || { echo "  FAIL  floor column: $f"; exit 1; }
  [ "$r" = "off" ]            && echo "  PASS  reads rate column"  || { echo "  FAIL  rate column: $r"; exit 1; }
  [ -z "$u" ]                 && echo "  PASS  unknown name is empty" || { echo "  FAIL  unknown name: $u"; exit 1; }
) || fail=$((fail+1))

echo
echo "--- ratchet lib: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ]
