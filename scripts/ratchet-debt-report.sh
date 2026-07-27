#!/usr/bin/env bash
# scripts/ratchet-debt-report.sh — the decay half of the ratchet design.
#
# The pre-commit ratchets fail only on GROWTH, so they freeze the level and
# never lower it. This script supplies the missing pressure: for each row in
# scripts/ratchet-config.tsv it derives a decay TARGET and reports how far the
# counter is above it.
#
# WHY THE RATE IS DERIVED, NOT DECLARED
# ------------------------------------
# A hand-picked "shed 25 clone groups per week" is a number nobody measured, and
# .claude/rules/no-number-without-a-measurement.md is explicit that a pipeline
# must not emit numbers it did not measure. So `rate=auto` reads the counter's
# OWN git history — every commit that changed its baseline constant — and fits
# the velocity that counter has actually demonstrated. The target is then a
# fraction (RATCHET_AGGRESSION, default 0.5) of proven velocity, which makes it
# achievable by construction: it only ever asks for half of what this repo has
# already shown it can do on this specific counter.
#
# The anchor is the timestamp the baseline LAST moved, so pressure accrues with
# staleness rather than on a fixed calendar. A counter improved yesterday has a
# target equal to its baseline (no pressure); one static since May accrues a
# target well below it. That directly targets the observed pathology — as of
# 2026-07-27, src/daemon.c had not moved off 14,132 LOC since May.
#
# EXIT STATUS
#   0  every configured counter is at or below its target (or has no target)
#   1  at least one counter is above target  -> the weekly workflow files an issue
#   2  a measurement failed  -> INCONCLUSIVE, never reported as debt
#
# Exit 2 exists because a script that cannot measure must refuse rather than
# emit a well-formed number (same rule as above). A parse failure must never be
# silently rendered as "0 debt".
#
# Deliberately NOT wired into .githooks/pre-commit: these gates block the merge
# queue (.claude/rules/ci-required-checks.md) and a calendar-driven failure
# would block commits that changed nothing relevant.
set -uo pipefail

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
CONFIG="scripts/ratchet-config.tsv"
AGGRESSION="${RATCHET_AGGRESSION:-0.5}"
HISTORY_DAYS="${RATCHET_HISTORY_DAYS:-180}"
JSON_OUT="${RATCHET_JSON_OUT:-}"

[ -f "$CONFIG" ] || { echo "missing $CONFIG" >&2; exit 2; }

# Validate the tunables before they reach awk. RATCHET_AGGRESSION is settable
# from a workflow_dispatch input, so it is external input by the time it lands
# here; a non-numeric value would silently produce nonsense targets rather than
# an error, which is precisely the "emit a number nobody measured" failure this
# report exists to avoid. Refuse instead.
# NOTE: `case "$v" in $pattern)` does NOT honor `|` alternation supplied through
# a variable — alternation is parsed syntactically, so the whole string becomes
# one literal pattern and every value is rejected, including the valid default.
# Validate with an explicit regex plus a numeric range instead.
_num_or_die() { # _num_or_die NAME VALUE REGEX MIN MAX
    if ! printf '%s' "$2" | grep -qE "$3"; then
        echo "invalid $1='$2' (must match $3)" >&2; exit 2
    fi
    if ! awk -v v="$2" -v lo="$4" -v hi="$5" 'BEGIN { exit !(v+0 >= lo && v+0 <= hi) }'; then
        echo "invalid $1='$2' (must be within [$4, $5])" >&2; exit 2
    fi
}
_num_or_die RATCHET_AGGRESSION    "$AGGRESSION"                    '^[0-9]+(\.[0-9]+)?$' 0.01 1
_num_or_die RATCHET_HISTORY_DAYS  "$HISTORY_DAYS"                  '^[0-9]+$'            1    3650
_num_or_die RATCHET_MAX_STEP_FRAC "${RATCHET_MAX_STEP_FRAC:-0.05}" '^[0-9]+(\.[0-9]+)?$' 0.01 1

now=$(date +%s)
over=0
inconclusive=0
rows=""

# baseline_history VAR SCRIPT -> "epoch value" per line, oldest first.
# Walks commits that touched the script and reads the constant out of each
# revision. Bounded by HISTORY_DAYS so a five-year repo does not pay for it.
baseline_history() {
    local var="$1" script="$2" sha val ts
    git log --since="${HISTORY_DAYS} days ago" --format='%H %ct' -- "$script" 2>/dev/null \
    | while read -r sha ts; do
        val=$(git show "${sha}:${script}" 2>/dev/null \
              | grep -oE "^${var}=[0-9]+" | head -1 | cut -d= -f2)
        [ -n "$val" ] && echo "$ts $val"
      done | sort -n
}

printf '%-20s %10s %10s %10s %9s  %s\n' RATCHET CURRENT BASELINE TARGET DEBT STATUS
printf '%s\n' "----------------------------------------------------------------------------------"

while IFS=$'\t' read -r name script var floor rate rule pattern; do
    case "$name" in ''|'#'*) continue ;; esac
    [ -f "$script" ] || continue

    baseline=$(grep -oE "^${var}=[0-9]+" "$script" 2>/dev/null | head -1 | cut -d= -f2)
    # Measure by running the gate and reading the integer before "(ceiling".
    current=$(bash "$script" 2>&1 | grep -E "$pattern" \
              | grep -oE '[0-9]+[^0-9]*\(ceiling' | grep -oE '^[0-9]+' | head -1)

    if [ -z "${baseline:-}" ] || [ -z "${current:-}" ]; then
        printf '%-20s %10s %10s %10s %9s  %s\n' \
            "$name" "${current:-?}" "${baseline:-?}" "-" "-" "INCONCLUSIVE (unparsed)"
        inconclusive=1
        continue
    fi

    if [ "$rate" = "off" ]; then
        printf '%-20s %10s %10s %10s %9s  %s\n' "$name" "$current" "$baseline" "-" "-" "no target (rate=off)"
        continue
    fi

    # --- derive velocity + anchor from this counter's own history ---
    hist=$(baseline_history "$var" "$script")
    anchor_ts=$(echo "$hist" | awk 'NF==2{t=$1} END{print t+0}')
    [ "${anchor_ts:-0}" -gt 0 ] 2>/dev/null || anchor_ts=$now

    # Observed velocity = MEDIAN of the per-drop rates, not total/elapsed.
    #
    # Total-over-elapsed is dominated by step changes: root-files fell 101 -> 4
    # in essentially one refactor, which that formula reported as a sustained
    # 3198 units/week and drove every target instantly to its floor. Taking the
    # median across individual drops makes one large carve a single sample
    # rather than the whole trend, and flooring each interval at one week stops
    # two same-day commits from implying an unbounded rate.
    velocity=$(echo "$hist" | awk -v agg="$AGGRESSION" '
        NF==2 {
            if (have) {
                dw = ($1 - pt) / 604800.0
                if (dw < 1.0) dw = 1.0          # same-day moves are not a weekly rate
                drop = pv - $2                  # positive when the counter fell
                if (drop > 0) r[++m] = drop / dw
            }
            pt = $1; pv = $2; have = 1
        }
        END {
            if (m < 1) { print "0"; exit }
            for (i = 1; i <= m; i++)            # insertion sort; m is tiny
                for (j = i+1; j <= m; j++)
                    if (r[j] < r[i]) { s = r[i]; r[i] = r[j]; r[j] = s }
            med = (m % 2) ? r[(m+1)/2] : (r[m/2] + r[m/2+1]) / 2.0
            printf "%.4f", med * agg
        }')

    weeks_idle=$(awk -v a="$anchor_ts" -v n="$now" 'BEGIN{printf "%.3f", (n-a)/604800.0}')
    # Cap any single evaluation's ask at MAX_STEP_FRAC of the baseline. Without
    # it, a counter left idle for months accrues an unpayable target in one jump
    # (sqlite-includers went straight from 97 to its floor of 0), which reads as
    # "give up" rather than "pay some down" and would make the weekly issue noise.
    target=$(awk -v b="$baseline" -v v="$velocity" -v w="$weeks_idle" -v f="$floor" \
                 -v cap="${RATCHET_MAX_STEP_FRAC:-0.05}" '
        BEGIN {
            ask = v * w
            maxask = b * cap
            if (ask > maxask) ask = maxask
            t = b - ask
            if (f != "none" && t < f+0) t = f+0
            if (t < 0) t = 0
            # Round the TARGET UP (== round the ask DOWN): a fractional ask must
            # not become a whole unit of debt. Without this, file-size showed
            # "OVER by 1" after 0.066 weeks idle — eleven hours of staleness
            # billed as a full line.
            ti = int(t); if (t > ti) ti++
            printf "%d", ti
        }')

    debt=$(( current - target ))
    if [ "$velocity" = "0" ]; then
        status="no proven velocity yet (needs 2+ baseline moves in ${HISTORY_DAYS}d)"
    elif [ "$debt" -gt 0 ]; then
        status="OVER by $debt — idle ${weeks_idle}w, rate $(printf '%.1f' "$velocity")/wk"
        over=1
    else
        status="on track (${weeks_idle}w idle)"
    fi
    printf '%-20s %10s %10s %10s %9s  %s\n' "$name" "$current" "$baseline" "$target" "$debt" "$status"
    rows="${rows}${name}|${current}|${baseline}|${target}|${debt}|${rule}\n"
done < "$CONFIG"

echo
if [ -n "$JSON_OUT" ]; then
    { echo '{"generated_at":'"$now"',"aggression":'"$AGGRESSION"',"ratchets":['
      printf "$rows" | awk -F'|' 'NF==6{ if(n++)printf ","; printf "{\"name\":\"%s\",\"current\":%s,\"baseline\":%s,\"target\":%s,\"debt\":%s,\"rule\":\"%s\"}",$1,$2,$3,$4,$5,$6 }'
      echo ']}'
    } > "$JSON_OUT"
fi

if [ "$inconclusive" = "1" ]; then
    echo "RESULT_ratchet_debt=INCONCLUSIVE (a counter could not be measured; no debt claimed)" >&2
    exit 2
fi
if [ "$over" = "1" ]; then
    echo "RESULT_ratchet_debt=OVER"
    echo "Pay down any counter marked OVER, or record why the target is wrong in $CONFIG."
    exit 1
fi
echo "RESULT_ratchet_debt=OK"
exit 0
