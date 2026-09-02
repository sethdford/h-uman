#!/usr/bin/env bash
# check-clone-ratchet.sh
#
# Clone-block duplication ratchet (DDD bounded-context refactor, Phase 0).
# Detects code duplication by counting sliding-window line patterns.
# Fails if count of duplicated windows grows past baseline.
#
# Approach: normalize lines (strip whitespace, drop comments/blanks),
# build overlapping windows of 6 consecutive lines, count window
# occurrences, report number of windows appearing 2+ times.
set -euo pipefail

# Measured 2026-05-31 at the start of Phase 0 (11766); re-measured and
# lowered 2026-07-12 after switching enumeration to git-tracked files —
# untracked/ignored generated blobs (e.g. stale embed-data output under
# src/data/) were inflating local counts by ~650 groups vs the committed
# tree, so working-tree runs and CI disagreed. Same pass added per-file
# boundary sentinels (windows no longer span files, killing enumeration-
# order dependence) and re-measured: 11587; 11552 after the
# immersive-section table refactor (prompt.c) landed the same day.
# 2026-07-18: origin/main itself measured 11557 (baseline had gone stale);
# the S2.1b carve merge lands at 11553 — a net -4 vs main with zero new
# groups (verified by set-diffing merged-tree windows against origin/main).
CLONE_BASELINE=11512   # measured 2026-09-01 after the graph_ingest reroute
                       # duplicated extras-dispatch preamble in reliable_chat and
                       # reliable_chat_with_system to one line each
# prior: 11515         # locked 2026-07-19: hu_file_slurp adopted by five more read
                       # sites (file_edit, pdf, image, meeting_transcribe, computer_use
                       # PNG reader), retiring their hand-rolled fopen/fseek/ftell
                       # preambles (was 11541 after origin's reflection
                       # PATTERN_QUERY_PREFIX dedup + imessage_caps module, see git log
                       # of this line for that history).
WINDOW=6

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

fail=0

# Tempfile
tmp_normalized=$(mktemp)
trap "rm -f '$tmp_normalized'" EXIT

# Extract and normalize all lines from src/**/*.c
{
    { git ls-files 'src/**/*.c' 'src/*.c' 2>/dev/null \
          || find src -name '*.c' -type f 2>/dev/null; } \
        | sort -u \
        | grep -v '/third_party/' \
        | grep -v '/vendor/' \
        | grep -v '_generated\.c$' \
        | while read -r file; do
            [ -f "$file" ] || continue
            # Per-file boundary sentinel: the window builder below runs over
            # one concatenated stream, so without this, windows SPAN file
            # boundaries and the count depends on enumeration order (find's
            # filesystem order vs sorted). The sentinel embeds the filename,
            # so boundary-crossing windows are unique and never count as
            # clones.
            printf '###FILE### %s\n' "$file"
            awk -v fname="$file" '
                NF == 0 { next }                          # blank lines
                /^[[:space:]]*\/\// { next }              # C++ comments
                /^[[:space:]]*\*/ { next }                # block comment lines
                /^[[:space:]]*\/\*/ { next }              # block comment start
                {
                    # Normalize: strip whitespace, collapse internal spaces
                    $0 = $0
                    gsub(/[[:space:]]+/, " ")
                    sub(/^[[:space:]]+/, "")
                    sub(/[[:space:]]+$/, "")
                    print $0
                }
            ' "$file"
        done
} > "$tmp_normalized"

# Count windows: build WINDOW-line blocks and track duplicates
clone_count=$(awk -v w="$WINDOW" '
BEGIN {
    for (i = 0; i < w; i++) buffer[i] = ""
    line_num = 0
    window_total = 0
    window_dupes = 0
}
{
    line_num++

    # Shift buffer
    for (i = 0; i < w - 1; i++) {
        buffer[i] = buffer[i + 1]
    }
    buffer[w - 1] = $0

    # Once we have w lines, emit the window
    if (line_num >= w) {
        # Build window string (join with newline)
        window_str = ""
        for (i = 0; i < w; i++) {
            if (i > 0) window_str = window_str "\n"
            window_str = window_str buffer[i]
        }
        # Count this window
        window_count[window_str]++
    }
}
END {
    # Count how many windows appear 2+ times
    dupes = 0
    for (window in window_count) {
        if (window_count[window] > 1) {
            dupes++
        }
    }
    print dupes
}
' "$tmp_normalized")

echo "Scanning src/**/*.c for code duplication (window=$WINDOW)..."
echo "Clone groups found: $clone_count (ceiling $CLONE_BASELINE)"

if [ "$clone_count" -gt "$CLONE_BASELINE" ]; then
    echo "FAIL: new clone blocks detected. Baseline: $CLONE_BASELINE, current: $clone_count" >&2
    echo "      Run deduplication to lower the count, then update CLONE_BASELINE." >&2
    fail=1
elif [ "$clone_count" -lt "$CLONE_BASELINE" ]; then
    echo "NOTE: clone count dropped to $clone_count — lower CLONE_BASELINE to lock the gain." >&2
fi

exit $fail
