#!/usr/bin/env bash
# scripts/lib/ratchet.sh — shared ratchet behavior, sourced by each check-*.sh.
#
# Provides the AUTO-LOCK half of the decay design (see scripts/ratchet-config.tsv
# for the rationale and the decay half).
#
# Every ratchet script already printed, on measuring below its baseline:
#     "NOTE: count dropped to N — lower BASELINE to lock the gain."
# That manual step was inconsistently followed, so incidental improvements
# leaked back as slack: on 2026-07-27 the clone ratchet carried 8 units of
# unlocked gain, while file-size / sqlite / root-files sat exactly on their
# ceilings. ratchet_autolock closes that loop mechanically.
#
# Deliberately NOT here: anything calendar-dependent. Pre-commit gates block the
# merge queue (.claude/rules/ci-required-checks.md), so a date boundary must
# never be able to fail a commit. All decay-target evaluation lives in
# scripts/ratchet-debt-report.sh, which only the weekly workflow enforces.

# ratchet_autolock VAR CURRENT SCRIPT_FILE
#
# When CURRENT is strictly below the baseline currently written in SCRIPT_FILE,
# rewrite that constant to CURRENT and stage the file, so the gain can never be
# spent again. Never fails the caller: auto-lock is a bonus, not a gate — a
# failure to tighten must not block a commit that was otherwise legal.
ratchet_autolock() {
    local var="$1" current="$2" script="$3"

    [ "${HU_RATCHET_NO_AUTOLOCK:-0}" = "1" ] && return 0
    [ -n "$var" ] && [ -n "$current" ] && [ -f "$script" ] || return 0
    # Integer guard: a measurement that failed and produced "" or "n/a" must not
    # be written into a baseline (.claude/rules/no-number-without-a-measurement.md
    # — never persist a number the pipeline did not actually measure).
    case "$current" in ''|*[!0-9]*) return 0 ;; esac

    local old
    old=$(grep -oE "^${var}=[0-9]+" "$script" 2>/dev/null | head -1 | cut -d= -f2) || return 0
    [ -n "$old" ] || return 0
    [ "$current" -lt "$old" ] 2>/dev/null || return 0

    git rev-parse --show-toplevel >/dev/null 2>&1 || return 0

    # Refuse to touch a script that already carries unstaged edits: staging it
    # would sweep someone else's in-flight work into this commit.
    if ! git diff --quiet -- "$script" 2>/dev/null; then
        echo "NOTE: $var dropped $old -> $current, but $script has unstaged edits;" >&2
        echo "      not auto-locking. Lower $var by hand once that settles." >&2
        return 0
    fi

    local today
    today=$(date +%Y-%m-%d)
    # Replace only the constant line; any continuation comments beneath it are
    # provenance history and are left alone.
    if perl -i -pe "s{^\Q${var}=${old}\E.*\$}{${var}=${current}   # auto-locked ${today} (was ${old})}" \
            "$script" 2>/dev/null; then
        git add -- "$script" 2>/dev/null || true
        # Advertise the lock so the caller can suppress its legacy
        # "lower BASELINE by hand" NOTE — printing both is contradictory.
        HU_RATCHET_LOCKED=1
        echo "AUTO-LOCK: $var tightened $old -> $current (staged $script)"
    fi
    return 0
}

# ratchet_config_field NAME FIELD
# Reads a column out of scripts/ratchet-config.tsv. FIELD is one of
# script|var|floor|rate|rule|pattern. Empty output means "not configured".
ratchet_config_field() {
    local name="$1" field="$2" cfg
    cfg="$(git rev-parse --show-toplevel 2>/dev/null || echo .)/scripts/ratchet-config.tsv"
    [ -f "$cfg" ] || return 0
    awk -F'\t' -v n="$name" -v f="$field" '
        /^#/ || NF < 7 { next }
        $1 == n {
            if (f == "script") print $2
            else if (f == "var")   print $3
            else if (f == "floor") print $4
            else if (f == "rate")  print $5
            else if (f == "rule")  print $6
            else if (f == "pattern") print $7
        }' "$cfg"
}
